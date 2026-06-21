#include "decode/decoder_manager.h"

#include <algorithm>
#include <cmath>

// Shared front-end IF rate and passband. ~250 kHz IF -> ±100 kHz usable
// coverage per sub-band; decoders within ±90 kHz of a sub-band centre share it.
static constexpr double kSubRateTarget = 250000.0;
static constexpr double kSubBW = 200000.0;

void DecoderManager::configure(double Fs, double centerHz)
{
    Fs_ = Fs;
    centerHz_ = centerHz;
}

void DecoderManager::start()
{
    if (run_.load())
        return;

    unsigned hw = std::thread::hardware_concurrency();
    int nWorkers = (hw > 2) ? (int)hw - 1 : 1;
    if (nWorkers > 8)
        nWorkers = 8;
    if (nWorkers < 1)
        nWorkers = 1;

    run_.store(true);
    workers_.clear();
    for (int i = 0; i < nWorkers; ++i)
        workers_.push_back(std::make_unique<Worker>());
    for (auto& w : workers_)
        w->thread = std::thread([this, wp = w.get()]() { workerLoop(wp); });

    audio_.start(8000); // 8 kHz mono voice output
}

void DecoderManager::stop()
{
    run_.store(false);
    for (auto& w : workers_)
        w->cv.notify_all();
    for (auto& w : workers_)
        if (w->thread.joinable())
            w->thread.join();
    workers_.clear();
    audio_.stop();
    voiceMonitorId_ = -1;
}

void DecoderManager::feed(const float* iq, int nComplex)
{
    if (!run_.load())
        return;
    for (auto& w : workers_)
    {
        if (w->count.load() == 0)
            continue;
        std::vector<float> block(iq, iq + (size_t)nComplex * 2);
        {
            std::lock_guard<std::mutex> lk(w->qMtx);
            if (w->queue.size() >= kMaxQueue)
            {
                w->queue.pop_front();
                drops_.fetch_add(1);
            }
            w->queue.push_back(std::move(block));
        }
        w->cv.notify_one();
    }
}

int DecoderManager::addDecoder(double freqHz, int baud)
{
    if (Fs_ <= 0.0 || workers_.empty())
        return -1;

    int id;
    {
        std::lock_guard<std::mutex> lk(idMtx_);
        id = nextId_++;
    }

    // 1) Try to join an existing sub-band that covers this frequency.
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
        {
            if (std::fabs(freqHz - sb->centerHz) < 0.36 * sb->subRate)
            {
                Decoder* dec = sb->decoders.emplace_back(std::make_unique<Decoder>(
                    sb->subRate, sb->centerHz, freqHz, baud, id, &log_, &suLog_, &audio_, &cassign_, &netTable_, &egcLog_)).get();
                w->count.fetch_add(1);
                if (baud == 8400 && voiceMonitorId_ < 0)
                {
                    dec->setMonitored(true);
                    voiceMonitorId_ = id;
                }
                if (baud == 8400)
                    dec->setRecording(recordOn_, recordDir_);
                return id;
            }
        }
    }

    // 2) No covering sub-band -> create a new one on the least-loaded worker.
    Worker* best = workers_[0].get();
    for (auto& w : workers_)
        if (w->count.load() < best->count.load())
            best = w.get();

    std::lock_guard<std::mutex> lk(best->dMtx);
    auto sb = std::make_unique<SubBand>(Fs_, centerHz_, freqHz, kSubRateTarget, kSubBW);
    Decoder* dec = sb->decoders.emplace_back(std::make_unique<Decoder>(
        sb->subRate, sb->centerHz, freqHz, baud, id, &log_, &suLog_, &audio_, &cassign_, &netTable_, &egcLog_)).get();
    if (baud == 8400 && voiceMonitorId_ < 0)
    {
        dec->setMonitored(true);
        voiceMonitorId_ = id;
    }
    if (baud == 8400)
        dec->setRecording(recordOn_, recordDir_);
    best->subbands.push_back(std::move(sb));
    best->count.fetch_add(1);
    return id;
}

void DecoderManager::removeDecoder(int channelId)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto sbIt = w->subbands.begin(); sbIt != w->subbands.end(); ++sbIt)
        {
            auto& decs = (*sbIt)->decoders;
            for (auto it = decs.begin(); it != decs.end(); ++it)
                if ((*it)->channelId() == channelId)
                {
                    decs.erase(it);
                    w->count.fetch_sub(1);
                    if (decs.empty())
                        w->subbands.erase(sbIt); // drop now-empty sub-band
                    if (channelId == voiceMonitorId_)
                        voiceMonitorId_ = -1;
                    return;
                }
        }
    }
}

void DecoderManager::setVoiceMonitor(int channelId)
{
    voiceMonitorId_ = channelId;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                d->setMonitored(d->isVoice() && d->channelId() == channelId);
    }
    audio_.clear();
}

void DecoderManager::setRecording(bool on, const std::string& dir)
{
    recordOn_ = on;
    if (!dir.empty())
        recordDir_ = dir;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->isVoice())
                    d->setRecording(on, recordDir_);
    }
}

int DecoderManager::recordingCount()
{
    int n = 0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->isVoice() && d->recordingNow())
                    ++n;
    }
    return n;
}

void DecoderManager::setDecoderFreq(int channelId, double freqHz)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->channelId() == channelId)
                {
                    d->setFreq(freqHz); // stays within the sub-band's IF window
                    return;
                }
    }
}

void DecoderManager::removeAll()
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        w->subbands.clear();
        w->count.store(0);
        std::lock_guard<std::mutex> ql(w->qMtx);
        w->queue.clear();
    }
}

int DecoderManager::decoderCount()
{
    int n = 0;
    for (auto& w : workers_)
        n += w->count.load();
    return n;
}

int DecoderManager::subbandCount()
{
    int n = 0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        n += (int)w->subbands.size();
    }
    return n;
}

std::vector<DecoderManager::Status> DecoderManager::status()
{
    std::vector<Status> out;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                out.push_back({d->channelId(), d->freqMHz(), d->baud(),
                               d->locked(), d->ebno(), d->msgCount(),
                               d->egcBer(), d->egcFrames()});
    }
    std::sort(out.begin(), out.end(),
              [](const Status& a, const Status& b) { return a.freqMHz < b.freqMHz; });
    return out;
}

int DecoderManager::getConstellation(int channelId, std::vector<float>& out, int maxPairs)
{
    std::vector<double> tmp((size_t)maxPairs * 2);
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->channelId() == channelId)
                {
                    int pairs = d->getConstellation(tmp.data(), maxPairs);
                    out.resize((size_t)pairs * 2);
                    for (int i = 0; i < pairs * 2; ++i)
                        out[i] = (float)tmp[i];
                    return pairs;
                }
    }
    out.clear();
    return 0;
}

void DecoderManager::workerLoop(Worker* w)
{
    while (run_.load())
    {
        std::vector<float> block;
        {
            std::unique_lock<std::mutex> lk(w->qMtx);
            w->cv.wait(lk, [&]() { return !run_.load() || !w->queue.empty(); });
            if (!run_.load())
                break;
            block = std::move(w->queue.front());
            w->queue.pop_front();
        }
        int nWide = (int)(block.size() / 2);

        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
        {
            // Shared front-end: decimate the wideband to the sub-band IF once.
            sb->subIQ.clear();
            sb->frontEnd.process(block.data(), nWide, sb->subIQ);
            int nSub = (int)(sb->subIQ.size() / 2);
            if (nSub <= 0)
                continue;
            // Cheap per-channel DDCs run from the IF stream.
            for (auto& d : sb->decoders)
                d->process(sb->subIQ.data(), nSub);
        }
    }
}

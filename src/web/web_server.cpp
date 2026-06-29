#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

#include "web/web_server.h"

#include "core/app.h"
#include "decode/decoder_manager.h"
#include "decode/message_log.h"
#include "decode/icao_country.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

// ---- JSON helpers ----

static void jsonStr(std::ostringstream& o, const std::string& s)
{
    o << '"';
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o << '\\' << c; }
        else if (c == '\n') { o << "\\n"; }
        else if (c == '\r') { o << "\\r"; }
        else if (c == '\t') { o << "\\t"; }
        else if ((unsigned char)c < 0x20) { char h[8]; std::snprintf(h, sizeof(h), "\\u%04x", (unsigned)c); o << h; }
        else { o << c; }
    }
    o << '"';
}

static std::string messageJson(const DecodedMessage& m)
{
    std::ostringstream o;
    o << "{\"t\":" << (int64_t)m.timeSec << ",\"f\":" << m.freqMHz << ",\"a\":" << m.aesId << ",\"d\":" << m.downlink;
    if (!m.label.empty()) { o << ",\"lb\":"; jsonStr(o, m.label); }
    if (!m.reg.empty())   { o << ",\"rg\":"; jsonStr(o, m.reg); }
    if (!m.text.empty())  { o << ",\"tx\":"; jsonStr(o, m.text); }
    if (!m.icao.empty())  { o << ",\"ic\":"; jsonStr(o, m.icao);
        uint32_t ih = (uint32_t)std::strtoul(m.icao.c_str(), nullptr, 16);
        const char* ccode = icaoCountry(ih);
        if (ccode) { o << ",\"cc\":"; jsonStr(o, ccode); }
        if (isMilitaryIcao(ih)) o << ",\"mi\":1";
    }
    if (m.hasPos)         { o << ",\"la\":" << m.lat << ",\"lo\":" << m.lon << ",\"al\":" << m.alt; }
    if (!m.decoded.empty()){ o << ",\"dc\":"; jsonStr(o, m.decoded); }
    o << '}';
    return o.str();
}

static std::string suJson(const DecodedMessage& m)
{
    std::ostringstream o;
    o << "{\"t\":" << (int64_t)m.timeSec << ",\"f\":" << m.freqMHz << ",\"st\":" << (int)m.suType << ",\"a\":" << m.aesId;
    if (!m.text.empty()) { o << ",\"tx\":"; jsonStr(o, m.text); }
    if (!m.hex.empty())  { o << ",\"hx\":"; jsonStr(o, m.hex); }
    o << '}';
    return o.str();
}

static std::string egcJson(const EgcMessage& m)
{
    std::ostringstream o;
    o << '{';
    bool first = true;
    if (!m.timeUtc.empty())  { if (!first) o << ','; o << "\"ut\":"; jsonStr(o, m.timeUtc); first = false; }
    if (!m.service.empty()) { if (!first) o << ','; o << "\"sv\":"; jsonStr(o, m.service); first = false; }
    if (!m.priority.empty()){ if (!first) o << ','; o << "\"pr\":"; jsonStr(o, m.priority); first = false; }
    if (!first) o << ','; o << "\"f\":" << m.freqMHz; first = false;
    if (!m.text.empty())    { if (!first) o << ','; o << "\"tx\":"; jsonStr(o, m.text); }
    o << '}';
    return o.str();
}

static std::string lesJson(const LesMessage& m)
{
    std::ostringstream o;
    o << '{';
    bool first = true;
    if (!m.timeUtc.empty())  { o << "\"ut\":"; jsonStr(o, m.timeUtc); first = false; }
    if (!m.satName.empty())  { if (!first) o << ','; o << "\"sn\":"; jsonStr(o, m.satName); first = false; }
    if (!m.lesLabel.empty()) { if (!first) o << ','; o << "\"ll\":"; jsonStr(o, m.lesLabel); first = false; }
    if (!first) o << ','; o << "\"f\":" << m.freqMHz; first = false;
    o << ",\"ch\":" << m.channel << ",\"pk\":" << m.pktNo << ",\"en\":" << (m.isEncrypted ? 1 : 0);
    if (!m.text.empty()) { o << ",\"tx\":"; jsonStr(o, m.text); }
    o << '}';
    return o.str();
}

static std::string voiceCallJson(const VoiceCallRecord& c)
{
    std::ostringstream o;
    o << "{\"t\":" << (int64_t)c.timeSec << ",\"d\":" << c.durationSec << ",\"f\":" << c.freqMHz << ",\"a\":" << c.aesId;
    if (!c.icao.empty())     { o << ",\"ic\":"; jsonStr(o, c.icao);
        uint32_t ih = (uint32_t)std::strtoul(c.icao.c_str(), nullptr, 16);
        const char* ccode = icaoCountry(ih);
        if (ccode) { o << ",\"cc\":"; jsonStr(o, ccode); }
        if (isMilitaryIcao(ih)) o << ",\"mi\":1";
    }
    if (!c.filename.empty()) { o << ",\"fn\":"; jsonStr(o, c.filename); }
    o << ",\"rc\":" << (c.recording ? 1 : 0) << '}';
    return o.str();
}

static std::string aircraftJson(const AircraftEntry& a)
{
    std::ostringstream o;
    o << "{\"a\":" << a.aesId;
    if (!a.icao.empty())   { o << ",\"ic\":"; jsonStr(o, a.icao);
        uint32_t ih = (uint32_t)std::strtoul(a.icao.c_str(), nullptr, 16);
        const char* ccode = icaoCountry(ih);
        if (ccode) { o << ",\"cc\":"; jsonStr(o, ccode); }
        if (isMilitaryIcao(ih)) o << ",\"mi\":1";
    }
    if (!a.reg.empty())    { o << ",\"rg\":"; jsonStr(o, a.reg); }
    if (!a.flight.empty()) { o << ",\"fl\":"; jsonStr(o, a.flight); }
    o << ",\"ms\":" << a.msgs;
    if (a.hasPos) { o << ",\"la\":" << a.lat << ",\"lo\":" << a.lon << ",\"al\":" << a.alt; }
    o << '}';
    return o.str();
}

static std::string decoderJson(const DecoderManager::Status& s)
{
    std::ostringstream o;
    o << "{\"id\":" << s.channelId << ",\"f\":" << s.freqMHz << ",\"bd\":" << s.baud;
    o << ",\"lk\":" << (s.locked ? 1 : 0) << ",\"eb\":" << s.ebno << ",\"ms\":" << (int64_t)s.msgs;
    o << ",\"vo\":" << (s.isVoice ? 1 : 0) << ",\"mo\":" << (s.monitored ? 1 : 0);
    if (s.egcCType) o << ",\"ct\":" << s.egcCType;
    o << '}';
    return o.str();
}

static std::string lesFreqJson(const LesFreqEntry& e)
{
    std::ostringstream o;
    o << "{\"f\":" << e.freqMHz << ",\"li\":" << e.lesId << ",\"sv\":" << e.services;
    if (!e.satName.empty())  { o << ",\"sn\":"; jsonStr(o, e.satName); }
    if (!e.lesLabel.empty()) { o << ",\"ll\":"; jsonStr(o, e.lesLabel); }
    o << ",\"ls\":" << (int64_t)e.lastSeen << ",\"hd\":" << (e.hasDecoder ? 1 : 0) << '}';
    return o.str();
}

static std::string jsonArray(const std::vector<std::string>& items)
{
    std::ostringstream o; o << '[';
    for (size_t i = 0; i < items.size(); ++i) { if (i) o << ','; o << items[i]; }
    o << ']';
    return o.str();
}

template <typename T, typename F>
static std::vector<std::string> mapTo(const std::vector<T>& src, F fn)
{
    std::vector<std::string> out; out.reserve(src.size());
    for (auto& v : src) out.push_back(fn(v));
    return out;
}

// ---- Dashboard HTML ----

static const char* kDashboardHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>InmarScope</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:14px system-ui,sans-serif;background:#0d0d1a;color:#c0c0d0;padding:8px}
h1{font-size:18px;color:#6080d0;margin-bottom:4px}
.tabs{display:flex;overflow-x:auto;gap:2px;margin:8px 0;padding-bottom:4px;-webkit-overflow-scrolling:touch}
.tab{padding:8px 14px;background:#1a1a30;border:1px solid #2a2a50;border-radius:6px 6px 0 0;cursor:pointer;white-space:nowrap;font-size:13px;color:#8080a0;min-height:44px;display:flex;align-items:center}
.tab.active{background:#16213e;color:#80a0ff;border-bottom-color:#16213e}
.toolbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:8px;background:#1a1a30;border-radius:0 6px 6px 6px;margin-bottom:8px}
.toolbar input{flex:1;min-width:120px;background:#0d0d1a;border:1px solid #2a2a50;color:#c0c0d0;padding:6px 10px;border-radius:4px;font-size:13px}
.toolbar button,.toolbar label{background:#16213e;border:1px solid #2a2a50;color:#80a0ff;padding:6px 12px;border-radius:4px;cursor:pointer;font-size:12px;white-space:nowrap;min-height:36px}
#content{background:#1a1a30;border-radius:6px;overflow-x:auto}
.table{width:100%;border-collapse:collapse;font-size:12px}
.table th{background:#16213e;color:#6080d0;padding:8px 10px;text-align:left;position:sticky;top:0;white-space:nowrap}
.table td{padding:6px 10px;border-bottom:1px solid #1a1a38;word-break:break-word;vertical-align:top}
.table tr:hover td{background:#1e1e3a}
.cards{display:none}
@media(max-width:768px){
  .table{display:none}
  .cards{display:block}
  .card{background:#16213e;border-radius:6px;padding:10px;margin-bottom:6px;cursor:pointer}
  .card .row{display:flex;justify-content:space-between;align-items:flex-start;font-size:12px;margin:2px 0}
  .card .row .label{color:#6080d0;margin-right:8px;white-space:nowrap}
  .card .row .val{color:#c0c0d0;text-align:right;word-break:break-word;flex:1}
  .card .row.collapsed{display:none}
  .card.open .row.collapsed{display:flex}
  .card .toggle{color:#6080d0;font-size:11px;text-align:right;padding-top:4px;min-height:30px;display:flex;align-items:center;justify-content:flex-end}
}
.enc{color:#ff4040}
.info{color:#6080d0;font-size:12px;padding:4px 0}
a{color:#80a0ff}
</style>
</head>
<body>
<h1>InmarScope Dashboard</h1>
<div id="tabs" class="tabs"></div>
<div class="toolbar">
  <input id="search" placeholder="Search..." oninput="renderTab()">
  <label><input type="checkbox" id="auto" checked onchange="setAuto()"> Auto</label>
  <button onclick="exportData()">Export JSON</button>
  <span class="info" id="info"></span>
</div>
<div id="content"></div>
<script>
var tab=0,tabs=['Messages','SUs','EGC','LES','Voice','Aircraft','Decoders','LES Freq'];
var data={},timer=null,openCards=new Set();
function init(){
  var d=document.getElementById('tabs');
  tabs.forEach(function(t,i){
    var e=document.createElement('div');
    e.className='tab'+(i===0?' active':'');
    e.textContent=t;e.onclick=function(){tab=i;renderTabs();fetchData();};
    d.appendChild(e);
  });
  fetchData();timer=setInterval(fetchData,2000);
}
function setAuto(){
  if(document.getElementById('auto').checked){if(!timer)timer=setInterval(fetchData,2000);}
  else{if(timer){clearInterval(timer);timer=null;}}
}
function renderTabs(){
  var d=document.getElementById('tabs').children;
  for(var i=0;i<d.length;i++) d[i].className='tab'+(i===tab?' active':'');
}
function fetchData(){
  var eps=['messages','sus','egc','les','voicecalls','aircraft','decoders','lesfreq'];
  fetch('/api/'+eps[tab]+'?limit=200').then(function(r){return r.json()}).then(function(j){
    data[eps[tab]]=j;renderTab();
  }).catch(function(){});
}
function esc(s){if(!s)return'';s=''+s;return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function fmtTime(t){return new Date(t*1000).toLocaleTimeString();}
function fmtDur(s){if(s<=0)return'--';var m=Math.floor(s/60),sec=Math.floor(s%60);return m+':'+(sec<10?'0':'')+sec;}
function renderTab(){
  var ep=tabs[tab].toLowerCase().replace(' ','');
  var items=data[ep]||[];
  var q=(document.getElementById('search').value||'').toLowerCase();
  if(q) items=items.filter(function(it){return JSON.stringify(it).toLowerCase().indexOf(q)>=0;});
  var c=document.getElementById('content');c.innerHTML='';
  if(window.innerWidth<=768) renderCards(items);else renderTable(items);
  document.getElementById('info').textContent=items.length+' rows @ '+new Date().toLocaleTimeString();
}
function renderTable(items){
  if(!items.length){document.getElementById('content').innerHTML='<div style="padding:12px;color:#8080a0">No data</div>';return;}
  var cols=getCols(),h='<table class="table"><thead><tr>';
  cols.forEach(function(c){h+='<th>'+c+'</th>';});
  h+='</tr></thead><tbody>';
  items.forEach(function(it,idx){
    h+='<tr>';
    cols.forEach(function(ci){
      var txt=cellHtml(ci,it);
      h+='<td>'+txt+'</td>';
    });
    h+='</tr>';
  });
  h+='</tbody></table>';
  document.getElementById('content').innerHTML=h;
}
function renderCards(items){
  if(!items.length){document.getElementById('content').innerHTML='<div style="padding:12px;color:#8080a0">No data</div>';return;}
  var cols=getCols(),h='<div class="cards">';
  items.forEach(function(it,idx){
    var id='c'+idx;
    var isOpen=openCards.has(id);
    h+='<div class="card'+(isOpen?' open':'')+'" id="'+id+'" onclick="toggleCard(\''+id+'\')">';
    cols.forEach(function(c,ci){
      var rowClass=ci<3?'':' collapsed';
      h+='<div class="row'+rowClass+'"><span class="label">'+c+'</span><span class="val">'+cellHtml(c,it)+'</span></div>';
    });
    h+='<div class="toggle">'+(isOpen?'\u25b2 collapse':'\u25b8 more')+'</div>';
    h+='</div>';
  });
  h+='</div>';
  document.getElementById('content').innerHTML=h;
}
function toggleCard(id){
  var card=document.getElementById(id);
  if(card){
    card.classList.toggle('open');
    var t=card.querySelector('.toggle');
    if(card.classList.contains('open')){
      openCards.add(id);
      if(t)t.textContent='\u25b2 collapse';
    }else{
      openCards.delete(id);
      if(t)t.textContent='\u25b8 more';
    }
  }
}
function getCols(){
  switch(tab){
    case 0:return['Time','Reg','Label','Text','Freq','ICAO'];
    case 1:return['Time','Freq','Text','SU','AES'];
    case 2:return['UTC','Service','Priority','Text','Freq'];
    case 3:return['UTC','Sat','LES','Text','Freq','Ch'];
    case 4:return['Time','Freq','ICAO','Duration','File'];
    case 5:return['AES','ICAO','Reg','Flight','Msgs','Pos'];
    case 6:return['ID','Freq','Baud','Locked','Eb/N0','Msgs','Type'];
    case 7:return['Freq','Sat','LES','Svc','Seen','Decoder'];
    default:return[];
  }
}
function ccToFlag(cc){
  if(!cc||cc.length!==2)return'';
  return String.fromCodePoint(0x1F1E6+cc.charCodeAt(0)-65)+String.fromCodePoint(0x1F1E6+cc.charCodeAt(1)-65);
}
function cellHtml(col,it){
  var v='';
  switch(col){
    case 'Time':v=fmtTime(it.t);break;
    case 'UTC':v=it.ut||'';break;
    case 'Freq':v=(it.f||0).toFixed(4)+' MHz';break;
    case 'Reg':v=esc(it.rg);break;
    case 'Label':v=esc(it.lb);break;
    case 'Text':v=esc(it.tx);if(it.st===48)v='<span class="org">'+v+'</span>';break;
    case 'ICAO':v=esc(it.ic)||(it.a?'0x'+it.a.toString(16).toUpperCase():'--');
      if(it.ic){
        var pf='';
        if(it.mi)pf='<span style="color:#ff4040">M</span> ';
        else if(it.cc)pf=ccToFlag(it.cc)+' ';
        v=pf+v;
      }
      break;
    case 'AES':v=it.a?'0x'+it.a.toString(16).toUpperCase():'--';break;
    case 'SU':v=it.st?'0x'+it.st.toString(16):'';break;
    case 'Service':v=esc(it.sv);break;
    case 'Priority':v=esc(it.pr);break;
    case 'Sat':v=esc(it.sn);break;
    case 'LES':v=esc(it.ll)||('LES '+it.li);break;
    case 'Ch':v=it.ch||'';break;
    case 'Duration':v=it.rc?('<span style="color:#ff4040">Rec</span> '+it.d.toFixed(0)+'s'):fmtDur(it.d);break;
    case 'File':v=esc(it.fn);break;
    case 'ID':v=it.id;break;
    case 'Baud':v=it.bd===1?'EGC':it.bd;break;
    case 'Locked':v=it.lk?'<span style="color:#40f040">YES</span>':'<span style="color:#8080a0">no</span>';break;
    case 'Eb/N0':v=(it.eb||0).toFixed(1);break;
    case 'Msgs':v=it.ms||0;break;
    case 'Type':v=it.vo?'Voice':(it.ct===1?'EGC NCS':(it.ct===2?'EGC LES':'Data'));break;
    case 'Svc':v='0x'+it.sv.toString(16).toUpperCase();break;
    case 'Seen':v=Math.max(0,Date.now()/1000-it.ls).toFixed(0)+'s ago';break;
    case 'Decoder':v=it.hd?'<span style="color:#40f040">ON</span>':'--';break;
    case 'Pos':v=(it.la!==undefined)?it.la.toFixed(3)+','+it.lo.toFixed(3)+' @'+it.al+'ft':'--';break;
    default:v=esc(it.tx)||esc(it.dc)||'';
  }
  return v||'';
}
function exportData(){
  var ep=tabs[tab].toLowerCase().replace(' ','');
  var blob=new Blob([JSON.stringify(data[ep]||[],null,2)],{type:'application/json'});
  var a=document.createElement('a');a.href=URL.createObjectURL(blob);
  a.download=ep+'_'+new Date().toISOString().slice(0,19).replace(/:/g,'-')+'.json';a.click();
}
window.onload=init;
</script>
</body>
</html>)html";

// ---- Mini HTTP server (WinSock / POSIX sockets) ----

static void sendResponse(SOCKET s, int code, const char* contentType, const std::string& body)
{
    char hdr[512];
    std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, contentType, body.size());
#if defined(_WIN32)
    ::send(s, hdr, (int)std::strlen(hdr), 0);
    ::send(s, body.c_str(), (int)body.size(), 0);
#else
    ::send(s, hdr, std::strlen(hdr), 0);
    ::send(s, body.c_str(), body.size(), 0);
#endif
}

static std::string getPath(const std::string& req)
{
    auto a = req.find(' ');
    if (a == std::string::npos) return "/";
    auto b = req.find(' ', a + 1);
    return req.substr(a + 1, b - a - 1);
}

static int getLimit(const std::string& path)
{
    int n = 200;
    auto p = path.find("limit=");
    if (p != std::string::npos)
    {
        n = std::atoi(path.c_str() + p + 6);
        if (n < 1) n = 1; if (n > 1000) n = 1000;
    }
    return n;
}

void WebServer::start(int port)
{
    stop();
    running_.store(true);
    thread_ = std::thread(&WebServer::serve, this, port);
}

void WebServer::stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

void WebServer::serve(int port)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return;

    int opt = 1;
#if defined(_WIN32)
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (::bind(ls, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(ls);
        return;
    }
    ::listen(ls, 8);

    while (running_.load())
    {
        struct timeval tv{0, 200000};
        fd_set fds; FD_ZERO(&fds); FD_SET(ls, &fds);
        if (::select((int)(ls + 1), &fds, nullptr, nullptr, &tv) <= 0)
            continue;

        SOCKET cl = ::accept(ls, nullptr, nullptr);
        if (cl == INVALID_SOCKET) continue;

        // Read request line + headers (loop if partial)
        char buf[8192] = {};
        int total = 0;
        while (total < (int)sizeof(buf) - 1)
        {
            int n = ::recv(cl, buf + total, (int)(sizeof(buf) - 1 - total), 0);
            if (n <= 0) break;
            total += n;
            buf[total] = 0;
            if (std::strstr(buf, "\r\n\r\n") || std::strstr(buf, "\n\n")) break;
        }
        if (total <= 0) { closesocket(cl); continue; }

        std::string req(buf);
        std::string path = getPath(req);

        // Route
        if (path == "/")
        {
            sendResponse(cl, 200, "text/html", kDashboardHtml);
        }
        else if (path.rfind("/api/", 0) == 0)
        {
            int limit = getLimit(path);
            std::string result;

            if (path.find("/api/messages") == 0)
            {
                auto items = decodersA ? decodersA->log().snapshot() : std::vector<DecodedMessage>{};
                if (decodersB && dualMode && *dualMode) { auto b = decodersB->log().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                std::sort(items.begin(), items.end(), [](auto& a, auto& b){return a.timeSec > b.timeSec;});
                if ((int)items.size() > limit) items.resize(limit);
                result = jsonArray(mapTo(items, messageJson));
            }
            else if (path.find("/api/sus") == 0)
            {
                auto items = decodersA ? decodersA->suLog().snapshot() : std::vector<DecodedMessage>{};
                if (decodersB && dualMode && *dualMode) { auto b = decodersB->suLog().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                std::sort(items.begin(), items.end(), [](auto& a, auto& b){return a.timeSec > b.timeSec;});
                if ((int)items.size() > limit) items.resize(limit);
                result = jsonArray(mapTo(items, suJson));
            }
            else if (path.find("/api/egc") == 0)
            {
                auto items = decodersA ? decodersA->egcLog().snapshot() : std::vector<EgcMessage>{};
                if (items.empty())
                {
                    // Return EGC-format test data so we can tell if the route works
                    result = "[{\"ut\":\"--:--:--\",\"sv\":\"(no EGC data yet)\",\"f\":0}]";
                }
                else
                {
                    if (decodersB && dualMode && *dualMode) { auto b = decodersB->egcLog().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                    std::reverse(items.begin(), items.end());
                    if ((int)items.size() > limit) items.resize(limit);
                    std::reverse(items.begin(), items.end());
                    result = jsonArray(mapTo(items, egcJson));
                }
            }
            else if (path.find("/api/lesfreq") == 0)
            {
                auto items = decodersA ? decodersA->lesFreqTable().snapshot() : std::vector<LesFreqEntry>{};
                if (decodersB && dualMode && *dualMode)
                {
                    auto b = decodersB->lesFreqTable().snapshot();
                    for (auto& e : b) {
                        bool dup = false;
                        for (auto& ea : items) if (std::fabs(ea.freqMHz - e.freqMHz) < 0.001) { dup = true; break; }
                        if (!dup) items.push_back(e);
                    }
                }
                std::sort(items.begin(), items.end(), [](auto& a, auto& b){return a.freqMHz < b.freqMHz;});
                result = jsonArray(mapTo(items, lesFreqJson));
            }
            else if (path.find("/api/les") == 0)
            {
                auto items = decodersA ? decodersA->lesLog().snapshot() : std::vector<LesMessage>{};
                if (items.empty())
                {
                    result = "[]";
                }
                else
                {
                    if (decodersB && dualMode && *dualMode) { auto b = decodersB->lesLog().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                    std::reverse(items.begin(), items.end());
                    if ((int)items.size() > limit) items.resize(limit);
                    std::reverse(items.begin(), items.end());
                    result = jsonArray(mapTo(items, lesJson));
                }
            }
            else if (path.find("/api/voicecalls") == 0)
            {
                auto items = decodersA ? decodersA->voiceCallLog().snapshot() : std::vector<VoiceCallRecord>{};
                if (decodersB && dualMode && *dualMode) { auto b = decodersB->voiceCallLog().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                std::sort(items.begin(), items.end(), [](auto& a, auto& b){return a.timeSec > b.timeSec;});
                result = jsonArray(mapTo(items, voiceCallJson));
            }
            else if (path.find("/api/aircraft") == 0)
            {
                auto items = decodersA ? decodersA->aircraftTable().snapshot() : std::vector<AircraftEntry>{};
                if (decodersB && dualMode && *dualMode) { auto b = decodersB->aircraftTable().snapshot(); items.insert(items.end(), b.begin(), b.end()); }
                std::sort(items.begin(), items.end(), [](auto& a, auto& b){return a.msgs > b.msgs;});
                result = jsonArray(mapTo(items, aircraftJson));
            }
            else if (path.find("/api/decoders") == 0)
            {
                auto items = decodersA ? decodersA->status() : std::vector<DecoderManager::Status>{};
                if (decodersB && dualMode && *dualMode) { auto b = decodersB->status(); items.insert(items.end(), b.begin(), b.end()); }
                result = jsonArray(mapTo(items, decoderJson));
            }
            else if (path.find("/api/status") == 0)
            {
                std::ostringstream o;
                o << '{';
                if (active && *active) {
                    o << "\"r\":" << ((*active)->running() ? 1 : 0);
                    o << ",\"fc\":" << (*active)->centerFreq();
                    o << ",\"fs\":" << (*active)->sampleRate();
                } else { o << "\"r\":0,\"fc\":0,\"fs\":0"; }
                if (decodersA) {
                    o << ",\"nd\":" << decodersA->decoderCount();
                    o << ",\"ns\":" << decodersA->subbandCount();
                    o << ",\"dr\":" << decodersA->drops();
                }
                o << '}';
                result = o.str();
            }
            else
            {
                sendResponse(cl, 404, "text/plain", "Not found");
                closesocket(cl);
                continue;
            }

            sendResponse(cl, 200, "application/json", result);
        }
        else
        {
            sendResponse(cl, 404, "text/plain", "Not found");
        }

        closesocket(cl);
    }

    closesocket(ls);
#if defined(_WIN32)
    WSACleanup();
#endif
}

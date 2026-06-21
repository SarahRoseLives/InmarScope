#include "decode/acars_apps.h"

#include <cstdio>
#include <cstring>
#include <mutex>

extern "C" {
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/adsc.h>
#include <libacars/list.h>
#include <libacars/vstring.h>
}

namespace {

// libacars initializes its configuration lazily and uses some lazily-built
// static dictionaries during parsing, so serialize all decode calls. ACARS
// messages are infrequent, so a single mutex is plenty.
std::mutex g_decodeMtx;

// Basic ADS-C report groups carrying lat/lon/alt (downlink tags 7,9,10,18,19,20).
bool isBasicReportTag(uint8_t t)
{
    return t == 7 || t == 9 || t == 10 || t == 18 || t == 19 || t == 20;
}

} // namespace

AcarsAppResult decodeAcarsApps(const std::string& label, const std::string& text,
                               bool downlink)
{
    AcarsAppResult r;
    if (label.empty() || text.empty())
        return r;

    std::lock_guard<std::mutex> lk(g_decodeMtx);

    la_msg_dir dir = downlink ? LA_MSG_DIR_AIR2GND : LA_MSG_DIR_GND2AIR;

    // Some labels (e.g. H1) prefix the application data with a sublabel/MFI;
    // skip past it so the right decoder sees clean payload.
    int offset = la_acars_extract_sublabel_and_mfi(label.c_str(), dir, text.c_str(),
                                                   (int)text.size(), nullptr, nullptr);
    if (offset < 0 || offset > (int)text.size())
        offset = 0;

    la_proto_node* node = la_acars_decode_apps(label.c_str(), text.c_str() + offset, dir);
    if (!node)
        return r;

    r.decoded = true;

    la_vstring* vs = la_proto_tree_format_text(nullptr, node);
    if (vs)
    {
        if (vs->str)
            r.text = vs->str;
        la_vstring_destroy(vs, true);
    }

    // Walk the ADS-C tags for position (basic report), flight id, airframe ICAO.
    // Only downlink (air->ground) messages carry these: the SAME tag numbers
    // (7/9/10/18/19/20) mean position reports on downlink but contract-request
    // structures on uplink, so casting an uplink tag to a basic report yields
    // garbage lat/lon/alt. Guard on direction and sanity-check the values.
    la_proto_node* adscNode = la_proto_tree_find_adsc(node);
    if (downlink && adscNode && adscNode->data)
    {
        la_adsc_msg_t* msg = static_cast<la_adsc_msg_t*>(adscNode->data);
        if (!msg->err)
        {
            for (la_list* l = msg->tag_list; l != nullptr; l = l->next)
            {
                la_adsc_tag_t* tag = static_cast<la_adsc_tag_t*>(l->data);
                if (!tag || !tag->data)
                    continue;
                if (!r.hasPos && isBasicReportTag(tag->tag))
                {
                    auto* br = static_cast<la_adsc_basic_report_t*>(tag->data);
                    if (br->lat >= -90.0 && br->lat <= 90.0 && br->lon >= -180.0 &&
                        br->lon <= 180.0 && br->alt > -1500 && br->alt < 70000)
                    {
                        r.hasPos = true;
                        r.lat = br->lat;
                        r.lon = br->lon;
                        r.alt = br->alt;
                    }
                }
                else if (tag->tag == 12) // flight id group
                {
                    auto* fid = static_cast<la_adsc_flight_id_t*>(tag->data);
                    r.flightId.assign(fid->id, strnlen(fid->id, sizeof(fid->id)));
                    // trim trailing spaces
                    while (!r.flightId.empty() && r.flightId.back() == ' ')
                        r.flightId.pop_back();
                }
                else if (tag->tag == 17) // airframe id group (ICAO 24-bit)
                {
                    auto* af = static_cast<la_adsc_airframe_id_t*>(tag->data);
                    char hx[8];
                    std::snprintf(hx, sizeof(hx), "%02X%02X%02X", af->icao_hex[0],
                                  af->icao_hex[1], af->icao_hex[2]);
                    r.icaoHex = hx;
                }
            }
        }
    }

    la_proto_tree_destroy(node);
    return r;
}

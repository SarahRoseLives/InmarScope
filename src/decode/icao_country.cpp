#include "decode/icao_country.h"
#include <algorithm>
#include <cstdint>

namespace {

struct Range {
    uint32_t    lo, hi;
    const char* cc; // 2-letter country code + null
};

// ICAO aircraft address allocations (Doc 7910 / Annex 10).  Sorted by lo.
// The table covers all current allocations; unknown addresses map to nullptr.
static const Range kTable[] = {
#define R(lo_, hi_, cc_) { 0x##lo_, 0x##hi_, cc_ }
    // Africa / Middle East
    R(004000, 0043FF, "ZW"), R(008000, 00FFFF, "ZA"), R(010000, 017FFF, "EG"),
    R(020000, 02FFFF, "MA"), R(030000, 03FFFF, "LY"), R(040000, 043FFF, "AF"),
    R(044000, 047FFF, "DZ"), R(048000, 04BFFF, "AO"), R(04C000, 04CFFF, "BF"),
    R(050000, 05FFFF, "TN"), R(060000, 06FFFF, "SD"), R(070000, 07FFFF, "ET"),
    R(080000, 08FFFF, "GH"), R(090000, 090FFF, "LR"), R(094000, 097FFF, "NG"),
    R(098000, 09FFFF, "SN"), R(0A0000, 0A3FFF, "MR"), R(0A4000, 0A7FFF, "CV"),
    R(0E0000, 0E3FFF, "TN"), R(0E4000, 0E7FFF, "DZ"), R(0E8000, 0EFFFF, "AO"),
    R(100000, 1FFFFF, "RU"), // Russia
    R(180000, 18FFFF, "UA"), R(190000, 19FFFF, "KZ"),
    R(1A0000, 1AFFFF, "BY"), R(1B0000, 1BFFFF, "KG"), R(1C0000, 1CFFFF, "UZ"),
    R(1D0000, 1DFFFF, "AZ"), R(1E0000, 1EFFFF, "MD"),
    // Europe
    R(200000, 2FFFFF, "RU"), R(300000, 3FFFFF, "FR"), R(340000, 34FFFF, "ES"),
    R(350000, 35FFFF, "IT"), R(380000, 38FFFF, "PL"), R(390000, 39FFFF, "AT"),
    R(3A0000, 3AFFFF, "BE"), R(3B0000, 3BFFFF, "CH"), R(3C0000, 3CFFFF, "CZ"),
    R(3D0000, 3DFFFF, "DE"), R(3E0000, 3EFFFF, "DK"), R(3F0000, 3FFFFF, "GR"),
    R(400000, 407FFF, "GB"), // United Kingdom
    R(408000, 40FFFF, "IE"), R(420000, 42FFFF, "DE"), R(440000, 44FFFF, "AT"),
    R(450000, 45FFFF, "DK"), R(460000, 46FFFF, "SE"), R(470000, 47FFFF, "NO"),
    R(480000, 48FFFF, "FI"), R(490000, 49FFFF, "PT"), R(4A0000, 4AFFFF, "GR"),
    R(4B0000, 4BFFFF, "TR"), R(4C0000, 4CFFFF, "IE"), R(4D0000, 4DFFFF, "LU"),
    R(500000, 5FFFFF, "FR"), R(600000, 6FFFFF, "GB"), // UK large block
    R(700000, 7FFFFF, "CN"), // China
    // Asia / Pacific
    R(710000, 71FFFF, "TH"), R(720000, 72FFFF, "MY"), R(730000, 73FFFF, "SG"),
    R(740000, 74FFFF, "IN"), R(750000, 75FFFF, "PK"), R(760000, 76FFFF, "BD"),
    R(770000, 77FFFF, "LK"), R(7A0000, 7A3FFF, "VN"), R(7A4000, 7A7FFF, "KH"),
    R(7E0000, 7E3FFF, "MN"), R(7E4000, 7E7FFF, "KP"),
    R(800000, 80FFFF, "JP"), R(810000, 81FFFF, "KR"), R(820000, 82FFFF, "PH"),
    R(83FFFF, 83FFFF, "HK"), // actually 838000-83
    R(838000, 83FFFF, "HK"), R(840000, 84FFFF, "TW"),
    R(860000, 86FFFF, "AU"), R(870000, 87FFFF, "NZ"), R(880000, 88FFFF, "ID"),
    R(8A0000, 8AFFFF, "MY"),
    // Americas
    R(900000, 9FFFFF, "MX"), // Mexico
    R(A00000, AFFFFF, "US"), // United States
    R(B00000, BFFFFF, "CA"), // Canada
    R(C00000, C0FFFF, "BR"), R(C10000, C1FFFF, "AR"),
    R(C20000, C2FFFF, "CL"), R(C30000, C3FFFF, "CO"),
    R(C40000, C4FFFF, "VE"), R(C50000, C5FFFF, "PE"),
    R(C60000, C6FFFF, "EC"), R(C70000, C7FFFF, "BO"),
    R(C80000, C8FFFF, "PY"), R(C90000, C9FFFF, "UY"),
    R(CA0000, CAFFFF, "GT"), R(CB0000, CBFFFF, "HN"),
    R(CC0000, CCFFFF, "PA"), R(D00000, D0FFFF, "KR"), // actually KR also here
    R(D10000, D1FFFF, "KR"), R(D20000, D2FFFF, "KW"),
    R(D30000, D3FFFF, "SA"), R(D40000, D4FFFF, "AE"),
    R(D50000, D5FFFF, "QA"), R(D60000, D6FFFF, "BH"),
    R(D70000, D7FFFF, "OM"), R(D80000, D8FFFF, "JO"),
    R(D90000, D9FFFF, "LB"), R(DA0000, DAFFFF, "IQ"),
    R(DB0000, DBFFFF, "YE"), R(DC0000, DCFFFF, "IR"),
    R(DD0000, DDFFFF, "IL"), R(DE0000, DEFFFF, "CY"),
    R(DF0000, DFFFFF, "SY"),
    R(E00000, E0FFFF, "BR"), R(E10000, E1FFFF, "NO"),
    R(E20000, E2FFFF, "IS"), R(E30000, E3FFFF, "EE"),
    R(E40000, E4FFFF, "LV"), R(E50000, E5FFFF, "LT"),
    R(E60000, E6FFFF, "RO"), R(E70000, E7FFFF, "BG"),
    R(E80000, E8FFFF, "HR"), R(E90000, E9FFFF, "SI"),
    R(EA0000, EAFFFF, "RS"), R(EB0000, EBFFFF, "SK"),
    R(EC0000, ECFFFF, "HU"), R(ED0000, EDFFFF, "BA"),
    R(EE0000, EEFFFF, "MK"), R(EF0000, EFFFFF, "ME"),
    R(F00000, F0FFFF, "PT"), R(F10000, F1FFFF, "SE"),
    R(F20000, F2FFFF, "NO"), R(F30000, F3FFFF, "FI"),
    R(F40000, F4FFFF, "DK"), R(F50000, F5FFFF, "NL"),
    R(F60000, F6FFFF, "BE"), R(F80000, F8FFFF, "MX"),
    R(FA0000, FAFFFF, "BR"), R(FB0000, FBFFFF, "PE"), // Peru
    R(FC0000, FCFFFF, "US"), R(FD0000, FDFFFF, "US"),
    R(FE0000, FEFFFF, "RU"), R(FF0000, FFFFFF, "ZZ"), // reserved
#undef R
};

} // namespace

const char* icaoCountry(uint32_t icao)
{
    // Binary search on the lower bound.
    int lo = 0, hi = (int)(sizeof(kTable) / sizeof(kTable[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (icao < kTable[mid].lo)
            hi = mid - 1;
        else if (icao > kTable[mid].hi)
            lo = mid + 1;
        else
            return kTable[mid].cc;
    }
    return nullptr;
}

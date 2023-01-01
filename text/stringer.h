#include <string>

std::string fieldBasedToString(int field);
std::string colorFamilyToString(int cf);
std::string chromaLocationToString(int location);
std::string rangeToString(int range);
std::string matrixToString(int matrix);
std::string primariesToString(int primaries);
std::string transferToString(int transfer);

#ifdef STRINGER_IMPL
std::string fieldBasedToString(int field) {
    std::string s;
    if (field == 0)
        s = "Frame based";
    else if (field == 1)
        s = "Bottom field first";
    else if (field == 2)
        s = "Top field first";
    else
        s = fmt::format("FieldBased({})", field);
    return s;
}

std::string colorFamilyToString(int cf) {
    std::string family;
    if (cf == cmGray)
        family = "Gray";
    else if (cf == cmRGB)
        family = "RGB";
    else if (cf == cmYUV)
        family = "YUV";
    else if (cf == cmYCoCg)
        family = "YCoCg";
    else if (cf == cmCompat)
        family = "Compat";
    else
        family = fmt::format("ColorFamily({})", cf);
    return family;
}

std::string chromaLocationToString(int location) {
    std::string s;
    if (location == 0)
        s = "Left";
    else if (location == 1)
        s = "Center";
    else if (location == 2)
        s = "Top left";
    else if (location == 3)
        s = "Top";
    else if (location == 4)
        s = "Bottom left";
    else if (location == 5)
        s = "Bottom";
    else
        s = fmt::format("Location({})", location);
    return s;
}

std::string rangeToString(int range) {
    std::string s;
    if (range == 0)
        s = "Full range";
    else if (range == 1)
        s = "Limited range";
    else
        s = fmt::format("Range({})", range);
    return s;
}

std::string matrixToString(int matrix) {
    std::string s;
    if (matrix == 0)
        s = "sRGB";
    else if (matrix == 1)
        s = "BT.709";
    else if (matrix == 4)
        s = "FCC";
    else if (matrix == 5 || matrix  == 6)
        s = "BT.601";
    else if (matrix == 7)
        s = "SMPTE 240M";
    else if (matrix == 8)
        s = "YCoCg";
    else if (matrix == 9)
        s = "BT.2020 NCL";
    else if (matrix == 10)
        s = "BT.2020 CL";
    else if (matrix == 11)
        s = "SMPTE 2085";
    else if (matrix == 12)
        s = "Cromaticity dervived cl";
    else if (matrix == 13)
        s = "Cromaticity dervived ncl";
    else if (matrix == 14)
        s = "ICtCp";
    else
        s = fmt::format("Matrix({})", matrix);
    return s;
}

std::string primariesToString(int primaries) {
    std::string s;
    if (primaries == 1)
        s = "BT.709";
    else if (primaries == 4)
        s = "BT.470M";
    else if (primaries == 5)
        s = "BT.470BG";
    else if (primaries == 6)
        s = "SMPTE 170M";
    else if (primaries == 7)
        s = "SMPTE 240M";
    else if (primaries == 8)
        s = "FILM";
    else if (primaries == 9)
        s = "BT.2020";
    else if (primaries == 10)
        s = "SMPTE 428";
    else if (primaries == 11)
        s = "SMPTE 431";
    else if (primaries == 12)
        s = "SMPTE 432";
    else if (primaries == 22)
        s = "JEDEC P22";
    else
        s = fmt::format("Primaries({})", primaries);
    return s;
}

std::string transferToString(int transfer) {
        std::string s;
        if (transfer == 1)
            s = "BT.709";
        else if (transfer == 4)
            s = "Gamma 2.2";
        else if (transfer == 5)
            s = "Gamma 2.8";
        else if (transfer == 6)
            s = "SMPTE 170M";
        else if (transfer == 7)
            s = "SMPTE 240M";
        else if (transfer == 8)
            s = "Linear";
        else if (transfer == 9)
            s = "Logaritmic (100:1 range)";
        else if (transfer == 10)
            s = "Logaritmic (100 * Sqrt(10) : 1 range)";
        else if (transfer == 11)
            s = "IEC 61966-2-4";
        else if (transfer == 12)
            s = "BT.1361 Extended Colour Gamut";
        else if (transfer == 13)
            s = "IEC 61966-2-1";
        else if (transfer == 14)
            s = "BT.2020 for 10 bit system";
        else if (transfer == 15)
            s = "BT.2020 for 12 bit system";
        else if (transfer == 16)
            s = "SMPTE 2084";
        else if (transfer == 17)
            s = "SMPTE 428";
        else if (transfer == 18)
            s = "ARIB STD-B67";
        else
            s = fmt::format("Transfer({})", transfer);
        return s;
}
#endif

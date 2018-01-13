
const char headersframeDefault[13][12] = {
    {0x00, 0x00, 0x3c, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01}, // ヘッダフレーム
    {0x00},                                                 // 圧縮情報
    {0x07, ':', 'm', 'e', 't', 'h', 'o', 'd'},              // 7 Byte :method
    {0x03, 'G', 'E', 'T'},                                  // 3 Byte GET
    {0x00},                                                 // 圧縮情報
    {0x05, ':', 'p', 'a', 't', 'h'},                        // 5 Byte :path
    {0x01, '/'},                                            // 1 /
    {0x00},                                                 // 圧縮情報
    {0x07, ':', 's', 'c', 'h', 'e', 'm', 'e'},              // 7 Byte :scheme
    {0x04, 'h', 't', 't', 'p'},                             // 4 Byte http
    {0x00},                                                 // 圧縮情報
    {0x0a, ':', "a", 'u', 't', 'h', 'o', 'r', 'i', 't', 'y'},      // 10 :authority
    {0x0b, 'n', 'g', 'h', 't', 't', 'p', '2', '.', 'o', 'r', 'g'}  // 11 nghttp2.org
};
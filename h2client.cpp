#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string>
#include <cstring>

#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#define SOCKET int
#define SD_BOTH SHUT_WR
#endif

#include "h2spec.h"

#define READ_BUF_SIZE 4096
#define BUF_SIZE 4097
#define PORT 80
#define BINARY_FRAME_LENGTH 9

#define pushPresetHeader(PTR, NUMBER, LENGTH) \
    memcpy(PTR, headersframeDefault[NUMBER], LENGTH); \
    PTR += LENGTH;

#define pushInputHeader(PTR, STR, LENGTH) \
    *PTR = LENGTH; \
    PTR += 1; \
    memcpy(PTR, STR, LENGTH);\
    PTR += LENGTH;



// 3バイトのネットワークオーダーを4バイト整数へ変換する関数.
char* to_framedata3byte(char *p, int &n);

int get_error();

void close_socket(SOCKET socket);

int send_data(SOCKET _socket, const char* data, size_t data_size);
int recv_data(SOCKET _socket, char* buffer, size_t buffer_limit);
void http2_main(SOCKET _socket, std::string hostname, std::string query);

using std::string;
int main(int argc, char **argv)
{

    //------------------------------------------------------------
    // 接続先ホスト名.
    // HTTP2に対応したホストを指定します.
    //------------------------------------------------------------

    string url;

    if (argc > 1) {
        url = string(argv[1]);
    } else {
        fprintf(stderr, "No argument\n");
        return 1;
    }

    string restOfUrl = url;
    if (url.find("http://") == 0) {
        restOfUrl = restOfUrl.substr(7, -1);
    } else if (url.find("https://") == 0) {
        restOfUrl = restOfUrl.substr(8, -1);
    } else {
        fprintf(stderr, "http or https reuqired\n");
        return 1;
    }

    string host, path;
    int hostOffset = restOfUrl.find("/");
    int port = PORT;
    if (hostOffset > 0) {
        string hostAndPort = restOfUrl.substr(0, hostOffset);
        int portOffset = hostAndPort.find(":");
        host = hostAndPort;
        if (portOffset > 0) {
            host = host.substr(0, portOffset);
            string portS = hostAndPort.substr(portOffset + 1, -1);
            port = atoi(portS.c_str());
        }
        restOfUrl = restOfUrl.substr(hostOffset, -1);
        path = restOfUrl;
    }
    printf("host=%s , port=%d , path=%s\n", host.c_str(), port, path.c_str());


    //------------------------------------------------------------
    // TCPの準備.
    //------------------------------------------------------------
#ifdef WIN32
    WSADATA wsaData;
    // WinSock?I‰?u‰≫.
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 0;
    }
#endif

    int error = 0;
    struct hostent *hp;
    struct sockaddr_in addr;
    SOCKET _socket;

    if (!(hp = gethostbyname(host.c_str()))){
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_addr = *(struct in_addr*)hp->h_addr_list[0];
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if ((_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))<0){
        return -1;
    }
    if (connect(_socket, (struct sockaddr *)&addr, sizeof(addr))<0){
        return -1;
    }

    try {
        // HTTP/2の処理は全てここで行う
        http2_main(_socket, host, string(path));
    } catch(string e) {
        fprintf(stderr, "Error => %s", e.c_str());
        ::shutdown(_socket, SD_BOTH);
        close_socket(_socket);
        return 1;
    }

    //------------------------------------------------------------
    // 後始末.
    //------------------------------------------------------------
    ::shutdown(_socket, SD_BOTH);
    close_socket(_socket);

    return 0;
}

void close_socket(SOCKET socket){
#ifdef WIN32
    ::closesocket(socket);
    WSACleanup();
#else
    ::close(socket);
#endif
}

int get_error(){
#ifdef WIN32
    return WSAGetLastError();
#endif
    return errno;
}

int send_data(SOCKET socket, const char* data, size_t data_size) {
    int r = (int)::send(socket, data, data_size, 0);
    if (r == -1){
        int error = get_error();
        std::string error_message = string("error code -> ");
        error_message += error;
        throw error_message;
    }
    return 0;
}

int recv_data(SOCKET socket, char* buffer, size_t buffer_limit) {
    int size = (int)::recv(socket, buffer, buffer_limit, 0);
    if (size == -1){
        int error = get_error();
        std::string error_message = string("error code -> ");
        error_message += error;
        throw error_message;
    }
    return size;
}

char* to_framedata3byte(char *p, int &n){
    u_char buf[4] = { 0 };
    memcpy(&(buf[1]), p, 3);
    memcpy(&n, buf, 4);
    n = ntohl(n);
    p += 3;
    return p;
}

void http2_main(SOCKET _socket, string hostname, string query)
{
    int result;
    char* p;
    //------------------------------------------------------------
    // HTTP2の準備.
    // SSLでない場合は、下記の24オクテットのデータを送信することで、
    // これからHTTP2通信を始めることを伝える.
    //------------------------------------------------------------
    string pri = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    send_data(_socket, pri.c_str(), pri.length());

    //------------------------------------------------------------
    // 全てのデータはバイナリフレームで送受信される
    // バイナリフレームは共通の9バイトヘッダと、データ本体であるpayloadを持つ
    //
    // ●ヘッダ部分のフォーマット
    //
    //   1-3バイト目  payloadの長さ。長さにヘッダの9バイトは含まれない。.
    //   4バイト目　フレームのタイプ.
    //   5バイト目　フラグ.
    //   6-9バイト目　ストリームID.(最初の1bitは予約で必ず0)
    //
    //  |Length(24bit)|Type(8bit)|Flags(8bit)|Reserve(1bit)|Stream Identifier(31bit)|
    //  |Frame Payload(Lengthバイト分)|
    //
    //
    // [フレームのタイプ]
    //
    // DATA(0x00)  リクエストボディや、レスポンスボディを転送する
    // HEADERS(0x01)  圧縮済みのHTTPヘッダーを転送する
    // PRIORITY(0x02)  ストリームの優先度を変更する
    // RST_STREAM(0x03)  ストリームの終了を通知する
    // SETTINGS(0x04)  接続に関する設定を変更する
    // PUSH_PROMISE(0x05)  サーバーからのリソースのプッシュを通知する
    // PING(0x06)  接続状況を確認する
    // GOAWAY(0x07)  接続の終了を通知する
    // WINDOW_UPDATE(0x08)   フロー制御ウィンドウを更新する
    // CONTINUATION(0x09)  HEADERSフレームやPUSH_PROMISEフレームの続きのデータを転送する
    //
    // それぞれのリクエストやレスポンスにはストリームIDが付与される.
    // クライアントから発行されるストリームIDは奇数.
    // サーバーから発行されるストリームIDは偶数.
    // ストリームには優先順位が付けられています.
    // 今回はストリームID「1」だけを使用します.
    //------------------------------------------------------------

    //------------------------------------------------------------
    // HTTP2通信のフロー
    //
    // まず最初にSettingフレームを必ず交換します.
    // Settingフレームを交換したら、設定を適用したことを伝えるために必ずACKを送ります.
    //
    // Client -> Server  SettingFrame
    // Client <- Server  SettingFrame
    // Client -> Server  ACK
    // Client <- Server  ACK
    //
    // Client -> Server  HEADERS_FRAME (GETなど)
    // Client <- Server  HEADERS_FRAME (ステータスコードなど)
    // Client <- Server  DATA_FRAME (Body)
    // 
    // Client -> Server  GOAWAY_FRAME (送信終了)
    //------------------------------------------------------------

    //------------------------------------------------------------
    // Settingフレームの送信.
    // フレームタイプは「0x04」
    // 全てデフォルト値を採用するためpayloadは空です。
    // SettingフレームのストリームIDは0です.(コネクション全体に適用されるため)
    //
    // 今回は空ですがSettingフレームのpayloadは次のフォーマットです.
    //
    // |Identifer(16bit)|Value(32bit)|
    // 上記を設定値の数だけ連結させ、最終的な長さをヘッダフレームのLengthに記述します.
    //
    // Identiferは次のものが定義されています。
    // SETTINGS_HEADER_TABLE_SIZE (0x1)  初期値は 4,096 オクテット
    // SETTINGS_ENABLE_PUSH (0x2)  初期値は1
    // SETTINGS_MAX_CONCURRENT_STREAMS (0x3)  初期状態では無制限
    // SETTINGS_INITIAL_WINDOW_SIZE (0x4)   初期値は 2^16-1 (65,535)
    // SETTINGS_MAX_FRAME_SIZE (0x5)    初期値は 2^24-1 (16777215)
    // SETTINGS_MAX_HEADER_LIST_SIZE (0x6)   初期値は無制限
    //------------------------------------------------------------
    const char settingframe[BINARY_FRAME_LENGTH] = { 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
    send_data(_socket, settingframe, BINARY_FRAME_LENGTH);


    //------------------------------------------------------------
    // Settingフレームの受信.
    //------------------------------------------------------------
    char buf[BUF_SIZE] = { 0 };
    p = buf;

    result = recv_data(_socket, p, READ_BUF_SIZE);


    //------------------------------------------------------------
    // ACKの送信.
    // ACKはSettingフレームを受け取った側が送る必要がある.
    // ACKはSettingフレームのフラグに0x01を立ててpayloadを空にしたもの.
    //
    // フレームタイプは「0x04」
    // 5バイト目にフラグ0x01を立てます。
    //------------------------------------------------------------
    const char settingframeAck[BINARY_FRAME_LENGTH] = { 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00 };

    send_data(_socket, settingframeAck, BINARY_FRAME_LENGTH);


    // サーバーからのACKの受信は下でやります..

    //------------------------------------------------------------
    // HEADERSフレームの送信.
    //
    // フレームタイプは「0x01」
    // このフレームに必要なヘッダがすべて含まれていてこれでストリームを終わらせることを示すために、
    // END_STREAM(0x1)とEND_HEADERS(0x4)を有効にします。
    // 具体的には5バイト目のフラグに「0x05」を立てます。
    // ストリームIDは「0x01」を使います.
    //
    // ここまででヘッダフレームは「ペイロードの長さ(3バイト), 0x01, 0x05, 0x00, 0x00, 0x00, 0x01」になります.
    //
    //
    // ●HTTP1.1でのセマンティクス
    // 　　"GET / HTTP/1.1"
    // 　　"Host: nghttp2.org
    //
    // ●HTTP2でのセマンティクス
    //      :method GET
    //      :path /
    //      :scheme http
    //      :authority nghttp2.org
    //
    // 本来HTTP2はHPACKという方法で圧縮します.
    // 今回は上記のHTTP2のセマンティクスを圧縮なしで記述します.
    //
    // 一つのヘッダフィールドの記述例
    //
    // |0|0|0|0|      0|   // 最初の4ビットは圧縮に関する情報、次の4ビットはヘッダテーブルのインデクス.(今回は圧縮しないのですべて0)
    // |0|            7|   // 最初の1bitは圧縮に関する情報(今回は0)、次の7bitはフィールドの長さ
    // |:method|           // フィールドをそのままASCIIのオクテットで書く。
    // |0|            3|   // 最初の1bitは圧縮に関する情報(今回は0)、次の7bitはフィールドの長さ
    // |GET|               // 値をそのままASCIIのオクテットで書く。
    //
    // 上記が一つのヘッダフィールドの記述例で、ヘッダーフィールドの数だけこれを繰り返す.
    //
    //------------------------------------------------------------

    int payloadSize = 1 + 8 + 4 + 1 + 6 + (1 + query.length()) + 1 + 8 + 5 + 1 + 11 + (1 + hostname.length());
    int headersFrameSize = payloadSize + HttpFrameHeaderSize;
    char* headersframeData = (char *)malloc(sizeof(char) * headersFrameSize);

    char* headersframeDataPtr = headersframeData;
    memcpy(headersframeDataPtr, headersframeDefault[0], HttpFrameHeaderSize);
    headersframeDataPtr[2] = payloadSize;
    headersframeDataPtr += HttpFrameHeaderSize;

    pushPresetHeader(headersframeDataPtr, 1, 1);
    pushPresetHeader(headersframeDataPtr, 2, 8);
    pushPresetHeader(headersframeDataPtr, 3, 4);
    pushPresetHeader(headersframeDataPtr, 4, 1);
    pushPresetHeader(headersframeDataPtr, 5, 6);
    pushInputHeader(headersframeDataPtr, query.c_str(), query.length());
    pushPresetHeader(headersframeDataPtr, 7, 1);
    pushPresetHeader(headersframeDataPtr, 8, 8);
    pushPresetHeader(headersframeDataPtr, 9, 5);
    pushPresetHeader(headersframeDataPtr, 10, 1);
    pushPresetHeader(headersframeDataPtr, 11, 11);
    pushInputHeader(headersframeDataPtr, hostname.c_str(), hostname.length());

    printf("headersFrameSize=%d\n", headersFrameSize);
    for (int i = 0;i < headersFrameSize;i++) {
        printf("%02x ", (unsigned int)(headersframeData[i] & 0xff));
        if (i % 16 == 15) {
            printf("\n");
        }
    }
    printf("\n");

    send_data(_socket, headersframeData, headersFrameSize);

    delete headersframeData;

    //------------------------------------------------------------
    // HEADERSフレームの受信.
    //------------------------------------------------------------
    int payload_length = 0;
    int frame_type = 0;

    printf("Receiving HEADERS frame\n");
    // まずはヘッダフレームを受信してpayloadのlengthを取得する。
    while (1){
        memset(buf, 0x00, BINARY_FRAME_LENGTH);
        p = buf;

        result = recv_data(_socket, p, BINARY_FRAME_LENGTH);

        printf("result => %d ", result);
        for (int i = 0;i < result;i++) {
            printf("%02x ", (unsigned int)(p[i] & 0xff));
            if (i % 16 == 15) {
                printf("\n");
            }
        }
        printf("\n");

        // ACKが返ってくる場合があるのでACKなら無視して次を読む。
        if (memcmp(buf, settingframeAck, BINARY_FRAME_LENGTH) == 0){
            continue;
        } else{

            // payloadの長さを取得する。
            p = to_framedata3byte(p, payload_length);

            // フレームタイプがHEADERS_FRAMEではなかったら読み飛ばす。
            memcpy(&frame_type, p, 1);
            if (frame_type != 1){
                result = recv_data(_socket, p, payload_length);
                continue;
            }
            break;
        }
    }

    // 次にHEADERSフレームのpayloadを受信する。
    memset(buf, 0x00, payload_length);
    p = buf;
    result = recv_data(_socket, p, payload_length);


    //------------------------------------------------------------
    // DATAフレームの受信.
    //------------------------------------------------------------

    // まずはヘッダフレームを受信してpayloadのlengthを取得する。
    memset(buf, 0x00, BINARY_FRAME_LENGTH);
    p = buf;
    result = recv_data(_socket, p, BINARY_FRAME_LENGTH);
    to_framedata3byte(p, payload_length);

    // 次にpayloadを受信する。
    while (payload_length > 0){

        memset(buf, 0x00, BUF_SIZE);
        p = buf;

        result = recv_data(_socket, p, READ_BUF_SIZE);
        payload_length -= result;

        printf("%s", p);
    }


    //------------------------------------------------------------
    // GOAWAYの送信.
    //
    // これ以上データを送受信しない場合はGOAWAYフレームを送信します.
    // フレームタイプは「0x07」
    // ストリームIDは「0x00」(コネクション全体に適用するため)
    //------------------------------------------------------------
    const char goawayframe[17] = { 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };

    send_data(_socket, goawayframe, 17);
}

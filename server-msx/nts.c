//=============================================================================
// nts.c — MSX Net Transfer 0.2.3 — HTTP server for MSX-DOS 2
//
// First implementation with HTTP: serves files from the current directory
// over HTTP/1.0. Matches the Rust server's protocol so NT.COM and curl can
// both speak to it. UI shell in SCREEN 0 80 columns, same look as NT.COM.
//
// Toggling S opens / closes the passive TCP listener. While SHARING is ON
// the main loop polls the listener each iteration; when a client connects
// the request is processed synchronously (UI is briefly unresponsive
// during the actual transfer — acceptable for one-MSX-at-a-time service).
//
// Endpoints:
//   GET /            -> tiny HTML index
//   GET /_list[?from=N&limit=N]  -> name<TAB>size<LF>... + X-Total-Count header
//   GET /<file>      -> file (Range: bytes=N- supported -> 206 Partial)
//   PUT /<file>      -> only when U toggle is on; .part + atomic rename
//
// Inspired by Konamiman's NestorWeb (passive-listener pattern) — no code.
//=============================================================================
#include "msxgl.h"
#include "dos.h"
#include "network.h"
#include "bios.h"
#include "bios_mainrom.h"
#include "bios_var.h"
#include "input.h"

#define NTS_VERSION      "0.2.3"
#define NTS_PORT         8088
#define MAX_FILES        256         // pagina del /_list
#define NAME_LEN         28
#define IDLE_LIMIT       20000

#define HTTP_HDR_MAX     1024        // header buffer (req + resp)
#define HTTP_BODY_CHUNK  1024        // chunk de body en streams

#define FILTER_ALL       0
#define FILTER_ROM       1
#define FILTER_DSK       2
#define FILTER_COUNT     3

//─────────────────────────────────────────────────────────────────
// Estado global (en BSS — inicializado a mano en main, ver nt.c)
//─────────────────────────────────────────────────────────────────
typedef struct {
    c8  name[NAME_LEN];
    u32 size;
} FileEntry;

static FileEntry g_Files[MAX_FILES];
static u16 g_FileCount;             // u16 — MAX_FILES=256 no cabe en u8
static u16 g_Selection;
static u16 g_ScrollTop;

static u8  g_FilterMode;
static u16 g_FilteredIdx[MAX_FILES];
static u16 g_FilteredCount;

static u8  g_LocalIp[4];
static bool g_NetReady;        // TRUE si UNAPI esta cargada y hay IP

static bool g_SharingOn;       // toggle S — TCP listener active
static bool g_UploadsOn;       // toggle U — PUT allowed
static u32 g_RequestCount;     // peticiones servidas desde arranque

// ── HTTP server state ──
static NetConn g_Listener = NET_INVALID_CONN;
static u8  g_HttpRxBuf[HTTP_BODY_CHUNK];   // recv scratch + disk read
static c8  g_HttpHdrBuf[HTTP_HDR_MAX];     // request headers acumulados
static u16 g_HttpHdrLen;
static c8  g_HttpResp[HTTP_HDR_MAX];       // response header build
static c8  g_HttpReqPath[80];              // path decodificado de la peticion
static c8  g_HttpReqMethod[8];
static u32 g_HttpReqContentLen;
static bool g_HttpReqHasRange;
static u32 g_HttpReqRangeStart;
static bool g_HttpReqForceOverwrite;       // If-Match: *

//─────────────────────────────────────────────────────────────────
// Layout de pantalla — calcado del cliente
//─────────────────────────────────────────────────────────────────
#define LIST_TOP_ROW     3
#define LIST_BOTTOM_ROW  20
#define ROWS_PER_COL     (LIST_BOTTOM_ROW - LIST_TOP_ROW + 1)
#define COLS_COUNT       2
#define LIST_VISIBLE     (ROWS_PER_COL * COLS_COUNT)
#define COL_WIDTH        40
#define NAME_VISIBLE     18
#define STATUS_ROW       23

//─────────────────────────────────────────────────────────────────
// CALSLT wrappers para BIOS — MISMOS que en nt.c
// (en una proxima refactorizacion se moveran a un header comun)
//─────────────────────────────────────────────────────────────────
static void Scr_Init80(void) __NAKED
{
__asm
    push ix
    push iy
    ld   a, #80
    ld   (#0xF3AE), a
    ld   iy, (#0xFCC0)
    ld   ix, #0x005F
    xor  a
    call #0x001C
    pop  iy
    pop  ix
    ret
__endasm;
}

static void Scr_Restore(void) __NAKED
{
__asm
    push ix
    push iy
    ld   a, #40
    ld   (#0xF3AE), a
    ld   iy, (#0xFCC0)
    ld   ix, #0x005F
    xor  a
    call #0x001C
    pop  iy
    pop  ix
    ret
__endasm;
}

static u8 My_Snsmat(u8 line) __NAKED
{
    (void)line;
__asm
    push ix
    push iy
    ld   iy, (#0xFCC0)
    ld   ix, #0x0141
    call #0x001C
    pop  iy
    pop  ix
    ret
__endasm;
}

//─────────────────────────────────────────────────────────────────
// Helpers de pantalla — mismos que nt.c
//─────────────────────────────────────────────────────────────────
static void Scr_Cls(void)            { DOS_CharOutput(0x0C); }
static void Scr_Locate(u8 x, u8 y)
{
    DOS_CharOutput(0x1B);
    DOS_CharOutput('Y');
    DOS_CharOutput(0x20 + y);
    DOS_CharOutput(0x20 + x);
}
static void Scr_EraseEOL(void) { DOS_CharOutput(0x1B); DOS_CharOutput('K'); }
static void Scr_PutChar(c8 c)  { DOS_CharOutput(c); }
static void Scr_PutStr(const c8* s) { while(*s) DOS_CharOutput(*s++); }
static void Scr_PutU32(u32 v)
{
    c8 tmp[12]; u8 i = 0;
    if(v == 0) tmp[i++] = '0';
    else { while(v) { tmp[i++] = '0' + (u8)(v % 10); v /= 10; } }
    while(i) DOS_CharOutput(tmp[--i]);
}
static void Scr_PutIP(const u8* ip)
{
    u8 k;
    for(k = 0; k < 4; k++) {
        c8 b[5]; u8 n = 0; u8 v = ip[k];
        if(v == 0) b[n++] = '0';
        else { while(v) { b[n++] = '0' + (v % 10); v /= 10; } }
        while(n--) DOS_CharOutput(b[n]);
        if(k < 3) DOS_CharOutput('.');
    }
}
static void Scr_PutU32Right(u32 v, u8 width)
{
    c8 tmp[12]; u8 i = 0;
    if(v == 0) tmp[i++] = '0';
    else { while(v) { tmp[i++] = '0' + (u8)(v % 10); v /= 10; } }
    while(i < width) { DOS_CharOutput(' '); width--; }
    while(i) DOS_CharOutput(tmp[--i]);
}
static void Scr_PutU16_3(u16 v)
{
    c8 d0, d1, d2;
    if(v > 999) v = 999;
    d2 = '0' + (v % 10); v /= 10;
    d1 = '0' + (v % 10); v /= 10;
    d0 = '0' + (u8)v;
    Scr_PutChar(d0 == '0' ? ' ' : d0);
    Scr_PutChar((d0 == ' ' && d1 == '0') ? ' ' : d1);
    Scr_PutChar(d2);
}
static void Scr_HLine(u8 y, c8 ch)
{
    u8 k;
    Scr_Locate(0, y);
    for(k = 0; k < 80; k++) DOS_CharOutput(ch);
}

//─────────────────────────────────────────────────────────────────
// Filtro de extension (mismo que el cliente)
//─────────────────────────────────────────────────────────────────
static u8 LowerCaseAt(const c8* s, u8 i)
{
    c8 c = s[i];
    if(c >= 'A' && c <= 'Z') c = c + 32;
    return (u8)c;
}

static bool FileMatchesFilter(const c8* name)
{
    const c8* dot = 0;
    const c8* p = name;
    if(g_FilterMode == FILTER_ALL) return TRUE;
    while(*p) { if(*p == '.') dot = p; p++; }
    if(!dot) return FALSE;
    dot++;
    if(g_FilterMode == FILTER_ROM) {
        return (LowerCaseAt(dot, 0) == 'r' &&
                LowerCaseAt(dot, 1) == 'o' &&
                LowerCaseAt(dot, 2) == 'm' &&
                dot[3] == 0);
    }
    return (LowerCaseAt(dot, 0) == 'd' &&
            LowerCaseAt(dot, 1) == 's' &&
            LowerCaseAt(dot, 2) == 'k' &&
            dot[3] == 0);
}

static void ApplyFilter(void)
{
    u8 i;
    g_FilteredCount = 0;
    for(i = 0; i < g_FileCount; i++) {
        if(FileMatchesFilter(g_Files[i].name))
            g_FilteredIdx[g_FilteredCount++] = i;
    }
    g_Selection = 0;
    g_ScrollTop = 0;
}

//─────────────────────────────────────────────────────────────────
// Enumera ficheros del directorio actual
//─────────────────────────────────────────────────────────────────
static void LocalListFill(void)
{
    DOS_FIB* fib;
    g_FileCount = 0;

    fib = DOS_FindFirstEntry("*.*", 0);
    while(fib && g_FileCount < MAX_FILES) {
        if((fib->Attribute & ATTR_FOLDER) == 0 &&
           (fib->Attribute & ATTR_DEVICE) == 0)
        {
            u8 k;
            for(k = 0; k < NAME_LEN - 1 && fib->Filename[k]; k++) {
                g_Files[g_FileCount].name[k] = fib->Filename[k];
            }
            g_Files[g_FileCount].name[k] = 0;
            g_Files[g_FileCount].size = fib->Size;
            g_FileCount++;
        }
        fib = DOS_FindNextEntry();
    }
    ApplyFilter();
}

//─────────────────────────────────────────────────────────────────
// Render de cabecera, badges, lista, ayuda
//─────────────────────────────────────────────────────────────────
static void Ui_DrawHeader(void)
{
    Scr_Locate(0, 0);
    Scr_EraseEOL();
    Scr_PutStr("NTS v" NTS_VERSION " - Net Transfer server");

    // Badges de estado — col 40
    Scr_Locate(40, 0);
    Scr_PutStr(g_SharingOn  ? "[SHARE ON ]" : "[SHARE OFF]");
    Scr_PutChar(' ');
    Scr_PutStr(g_UploadsOn  ? "[UP ON ]" : "[UP OFF]");
    Scr_PutChar(' ');
    if(g_FilterMode == FILTER_ROM)      Scr_PutStr("[ROM]");
    else if(g_FilterMode == FILTER_DSK) Scr_PutStr("[DSK]");
    else                                Scr_PutStr("[ALL]");
}

static void Ui_DrawIpLine(void)
{
    Scr_Locate(0, 1);
    Scr_EraseEOL();
    Scr_PutStr("Connect from PC:  http://");
    if(g_NetReady) {
        Scr_PutIP(g_LocalIp);
        Scr_PutChar(':');
        Scr_PutU32((u32)NTS_PORT);
        Scr_PutChar('/');
    } else {
        Scr_PutStr("(UNAPI not loaded -- run UNAPINET first)");
    }
}

static void ItemPos(u8 idx, u8* outX, u8* outY)
{
    u8 rel = idx - g_ScrollTop;
    u8 col = rel / ROWS_PER_COL;
    u8 row = rel % ROWS_PER_COL;
    *outX = col * COL_WIDTH;
    *outY = LIST_TOP_ROW + row;
}

static void Ui_DrawListItem(u8 idx)
{
    u8 x, y, k;
    FileEntry* fe = &g_Files[g_FilteredIdx[idx]];
    ItemPos(idx, &x, &y);
    Scr_Locate(x, y);
    if(idx == g_Selection) Scr_PutChar('>'); else Scr_PutChar(' ');
    Scr_PutChar(' ');
    for(k = 0; k < NAME_VISIBLE && fe->name[k]; k++) Scr_PutChar(fe->name[k]);
    while(k < NAME_VISIBLE) { Scr_PutChar(' '); k++; }
    Scr_PutChar(' ');
    Scr_PutU32Right(fe->size, 8);
    Scr_PutStr("    ");
}

static void Ui_DrawCursorAt(u8 idx, bool selected)
{
    u8 x, y;
    ItemPos(idx, &x, &y);
    Scr_Locate(x, y);
    Scr_PutChar(selected ? '>' : ' ');
}

static void Ui_UpdateCounter(void)
{
    Scr_Locate(0, STATUS_ROW);
    Scr_EraseEOL();
    if(g_FilteredCount == 0) {
        Scr_PutStr(g_FileCount == 0 ? "(no files in folder)" : "(no files match filter)");
    } else {
        Scr_PutStr("File ");
        Scr_PutU16_3((u16)(g_Selection + 1));
        Scr_PutStr(" of ");
        Scr_PutU16_3((u16)g_FilteredCount);
    }
    // Contador de peticiones a la derecha
    Scr_Locate(54, STATUS_ROW);
    Scr_PutStr("Requests served: ");
    Scr_PutU32(g_RequestCount);
}

static void Ui_DrawList(void)
{
    u8 i;
    u16 end16 = (u16)g_ScrollTop + (u16)LIST_VISIBLE;
    u8 end = (end16 > (u16)g_FilteredCount) ? g_FilteredCount : (u8)end16;

    for(i = LIST_TOP_ROW; i <= LIST_BOTTOM_ROW; i++) {
        Scr_Locate(0, i);
        Scr_EraseEOL();
    }
    for(i = g_ScrollTop; i < end; i++) Ui_DrawListItem(i);

    Scr_Locate(0, 2);
    Scr_EraseEOL();
    Scr_PutStr("  FILE                  SIZE             FILE                  SIZE");

    Ui_UpdateCounter();
}

static void Ui_DrawHelp(void)
{
    Scr_HLine(21, '-');
    Scr_Locate(0, 22);
    Scr_EraseEOL();
    Scr_PutStr("Arrows  S share  U uploads  F filter  D folder  R refresh  ESC exit");
}

static void Ui_DrawFrame(void)
{
    Scr_Cls();
    Ui_DrawHeader();
    Ui_DrawIpLine();
    Ui_DrawList();
    Ui_DrawHelp();
}

//─────────────────────────────────────────────────────────────────
// Movimiento del cursor (mismo patron que cliente)
//─────────────────────────────────────────────────────────────────
static void MoveSelection(u8 newSel)
{
    u8 prev = g_Selection;
    g_Selection = newSel;

    if(g_Selection < g_ScrollTop) {
        while(g_Selection < g_ScrollTop) g_ScrollTop -= ROWS_PER_COL;
        Ui_DrawList();
    } else if(g_Selection >= (u8)(g_ScrollTop + LIST_VISIBLE)) {
        while(g_Selection >= (u8)(g_ScrollTop + LIST_VISIBLE)) g_ScrollTop += ROWS_PER_COL;
        Ui_DrawList();
    } else {
        Ui_DrawCursorAt(prev, FALSE);
        Ui_DrawCursorAt(g_Selection, TRUE);
        Ui_UpdateCounter();
    }
}

//─────────────────────────────────────────────────────────────────
// Anti-bounce de teclas
//─────────────────────────────────────────────────────────────────
static void WaitKeyRelease(void)
{
    u8 row3, row4, row5, row7, row8;
    while(1) {
        row3 = My_Snsmat(3);   // KEY_C, KEY_D, KEY_F
        row4 = My_Snsmat(4);   // KEY_L, KEY_R, KEY_O
        row5 = My_Snsmat(5);   // KEY_S, KEY_U
        row7 = My_Snsmat(7);
        row8 = My_Snsmat(8);
        if((row3 & (KEY_FLAG(KEY_F))) == KEY_FLAG(KEY_F) &&
           (row4 & (KEY_FLAG(KEY_R) | KEY_FLAG(KEY_L))) ==
                  (KEY_FLAG(KEY_R) | KEY_FLAG(KEY_L)) &&
           (row5 & (KEY_FLAG(KEY_S) | KEY_FLAG(KEY_U))) ==
                  (KEY_FLAG(KEY_S) | KEY_FLAG(KEY_U)) &&
           (row7 & (KEY_FLAG(KEY_RETURN) | KEY_FLAG(KEY_ESC))) ==
                  (KEY_FLAG(KEY_RETURN) | KEY_FLAG(KEY_ESC)) &&
           (row8 & (KEY_FLAG(KEY_UP) | KEY_FLAG(KEY_DOWN) |
                    KEY_FLAG(KEY_LEFT) | KEY_FLAG(KEY_RIGHT))) ==
                  (KEY_FLAG(KEY_UP) | KEY_FLAG(KEY_DOWN) |
                    KEY_FLAG(KEY_LEFT) | KEY_FLAG(KEY_RIGHT)))
            return;
    }
}

//═════════════════════════════════════════════════════════════════
// HTTP server
//═════════════════════════════════════════════════════════════════

// ── helpers de envio ──
static bool HttpSendStr(NetConn conn, const c8* s)
{
    u16 len = 0;
    while(s[len]) len++;
    return Net_Send(conn, (const u8*)s, len) == NET_OK;
}

static bool HttpSendU32(NetConn conn, u32 v)
{
    c8 tmp[12]; u8 n = 0;
    if(v == 0) tmp[n++] = '0';
    else { while(v) { tmp[n++] = '0' + (u8)(v % 10); v /= 10; } }
    // tmp esta en orden inverso
    c8 buf[12]; u8 i = 0;
    while(n) buf[i++] = tmp[--n];
    return Net_Send(conn, (const u8*)buf, i) == NET_OK;
}

// Util: decimal -> u32. Devuelve TRUE si parseo OK.
static bool ParseU32Str(const c8* s, u32* out)
{
    u32 v = 0;
    if(!*s) return FALSE;
    while(*s >= '0' && *s <= '9') { v = v*10 + (u8)(*s - '0'); s++; }
    *out = v;
    return TRUE;
}

// URL decode in-place. Solo soporta %XX. Devuelve nueva longitud.
static u8 UrlDecodeInPlace(c8* s)
{
    u8 i = 0, j = 0;
    while(s[i]) {
        c8 c = s[i++];
        if(c == '%' && s[i] && s[i+1]) {
            u8 hi = (s[i]   <= '9') ? s[i]   - '0' :
                    (s[i]   <= 'F') ? s[i]   - 'A' + 10 :
                                       s[i]   - 'a' + 10;
            u8 lo = (s[i+1] <= '9') ? s[i+1] - '0' :
                    (s[i+1] <= 'F') ? s[i+1] - 'A' + 10 :
                                       s[i+1] - 'a' + 10;
            s[j++] = (c8)((hi << 4) | lo);
            i += 2;
        } else {
            s[j++] = c;
        }
    }
    s[j] = 0;
    return j;
}

// Path validation para PUT y GET. El nombre debe ser un solo componente,
// sin '/', '\', "..", y no empezar con '.'. Si OK escribe el nombre en
// nameOut (max 16 chars 8.3 con NUL) y devuelve TRUE.
static bool SafeFilename(const c8* path, c8* nameOut)
{
    const c8* p = path;
    u8 k = 0;
    if(*p == '/') p++;
    if(!*p) return FALSE;
    while(*p) {
        c8 c = *p++;
        if(c == '/' || c == '\\') return FALSE;
        if(c == '.' && k == 0) return FALSE;   // no leading dot
        if(k >= 15) return FALSE;              // 12 + reserva
        nameOut[k++] = c;
    }
    nameOut[k] = 0;
    // Rechazar ".."
    if(nameOut[0] == '.' && nameOut[1] == '.' && nameOut[2] == 0) return FALSE;
    return TRUE;
}

// Compara prefijo case-insensitive
static bool StartsWithI(const c8* h, const c8* prefix)
{
    while(*prefix) {
        c8 a = *h; c8 b = *prefix;
        if(a >= 'A' && a <= 'Z') a += 32;
        if(b >= 'A' && b <= 'Z') b += 32;
        if(a != b) return FALSE;
        h++; prefix++;
    }
    return TRUE;
}

// ── leer headers ──
// Acumula bytes del socket en g_HttpHdrBuf hasta encontrar \r\n\r\n,
// o hasta timeout / overflow.
// CRITICO: leemos byte a byte para no consumir bytes del body que pudieran
// venir en el mismo TCP packet. Si chupasemos un chunk grande, los bytes
// despues de \r\n\r\n se perderian — esto rompia PUT con bodies pequenos.
static bool HttpReadHeaders(NetConn conn)
{
    u16 idle = 0;
    g_HttpHdrLen = 0;
    while(1) {
        u16 avail = Net_Available(conn);
        if(avail == 0) {
            if(!Net_IsConnected(conn)) return FALSE;
            if(++idle > IDLE_LIMIT) return FALSE;
            continue;
        }
        idle = 0;
        u8 b;
        if(Net_Recv(conn, &b, 1) == 0) return FALSE;
        if(g_HttpHdrLen >= HTTP_HDR_MAX) return FALSE;
        g_HttpHdrBuf[g_HttpHdrLen++] = (c8)b;
        if(g_HttpHdrLen >= 4 &&
           g_HttpHdrBuf[g_HttpHdrLen-4] == '\r' &&
           g_HttpHdrBuf[g_HttpHdrLen-3] == '\n' &&
           g_HttpHdrBuf[g_HttpHdrLen-2] == '\r' &&
           g_HttpHdrBuf[g_HttpHdrLen-1] == '\n')
        {
            return TRUE;
        }
    }
}

// Parsea la primera linea (method, path) y algunos headers que nos importan.
// Devuelve TRUE si la peticion es minimamente valida.
static bool HttpParseRequest(void)
{
    u16 i = 0;
    // Method
    u8 mi = 0;
    while(i < g_HttpHdrLen && g_HttpHdrBuf[i] != ' ' && mi < 7) {
        g_HttpReqMethod[mi++] = g_HttpHdrBuf[i++];
    }
    g_HttpReqMethod[mi] = 0;
    if(g_HttpHdrBuf[i] != ' ') return FALSE;
    i++;
    // Path
    u8 pi = 0;
    while(i < g_HttpHdrLen && g_HttpHdrBuf[i] != ' ' && g_HttpHdrBuf[i] != '\r' && pi < 79) {
        g_HttpReqPath[pi++] = g_HttpHdrBuf[i++];
    }
    g_HttpReqPath[pi] = 0;
    if(pi == 0) return FALSE;

    // Headers — parsea solo los que nos interesan
    g_HttpReqContentLen = 0;
    g_HttpReqHasRange = FALSE;
    g_HttpReqRangeStart = 0;
    g_HttpReqForceOverwrite = FALSE;

    while(i < g_HttpHdrLen) {
        // Avanzar a inicio de linea siguiente
        while(i < g_HttpHdrLen && g_HttpHdrBuf[i] != '\n') i++;
        if(i >= g_HttpHdrLen) break;
        i++;   // tras el \n
        if(g_HttpHdrBuf[i] == '\r') break;  // linea vacia → fin headers
        if(StartsWithI(&g_HttpHdrBuf[i], "content-length:")) {
            u16 j = i + 15;
            while(j < g_HttpHdrLen && (g_HttpHdrBuf[j] == ' ' || g_HttpHdrBuf[j] == '\t')) j++;
            ParseU32Str(&g_HttpHdrBuf[j], &g_HttpReqContentLen);
        } else if(StartsWithI(&g_HttpHdrBuf[i], "range:")) {
            // bytes=N-... — extraemos solo N
            u16 j = i + 6;
            while(j < g_HttpHdrLen && (g_HttpHdrBuf[j] == ' ' || g_HttpHdrBuf[j] == '\t')) j++;
            if(StartsWithI(&g_HttpHdrBuf[j], "bytes=")) {
                j += 6;
                u32 start;
                if(ParseU32Str(&g_HttpHdrBuf[j], &start)) {
                    g_HttpReqHasRange = TRUE;
                    g_HttpReqRangeStart = start;
                }
            }
        } else if(StartsWithI(&g_HttpHdrBuf[i], "if-match:")) {
            u16 j = i + 9;
            while(j < g_HttpHdrLen && (g_HttpHdrBuf[j] == ' ' || g_HttpHdrBuf[j] == '\t')) j++;
            if(g_HttpHdrBuf[j] == '*') g_HttpReqForceOverwrite = TRUE;
        }
    }
    return TRUE;
}

// Extrae query param numerico del path. Devuelve TRUE si encontrado.
static bool HttpQueryParamU32(const c8* name, u32* out)
{
    u8 nameLen = 0;
    while(name[nameLen]) nameLen++;
    // Busca '?' en path
    c8* p = g_HttpReqPath;
    while(*p && *p != '?') p++;
    if(!*p) return FALSE;
    p++;
    while(*p) {
        // Match nombre seguido de '='
        u8 k = 0;
        while(k < nameLen && p[k] == name[k]) k++;
        if(k == nameLen && p[k] == '=') {
            p += k + 1;
            return ParseU32Str(p, out);
        }
        // Saltar hasta '&' o fin
        while(*p && *p != '&') p++;
        if(*p == '&') p++;
    }
    return FALSE;
}

// Strip query string del path para procesado del fichero
static void HttpStripQuery(void)
{
    c8* p = g_HttpReqPath;
    while(*p) { if(*p == '?') { *p = 0; return; } p++; }
}

// ── respuestas de error ──
static void HttpSendError(NetConn conn, u16 code, const c8* reason, const c8* body)
{
    u16 bodyLen = 0;
    while(body[bodyLen]) bodyLen++;
    HttpSendStr(conn, "HTTP/1.0 ");
    HttpSendU32(conn, (u32)code);
    HttpSendStr(conn, " ");
    HttpSendStr(conn, reason);
    HttpSendStr(conn, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    HttpSendU32(conn, (u32)bodyLen);
    HttpSendStr(conn, "\r\nConnection: close\r\n\r\n");
    HttpSendStr(conn, body);
}

// ── GET / — pagina HTML simple ──
static void HttpHandleGetRoot(NetConn conn)
{
    static const c8 body[] =
        "<!doctype html><html><head><meta charset=\"ascii\">"
        "<title>NTS MSX</title></head><body style=\"font-family:monospace\">"
        "<h2>MSX Net Transfer server</h2>"
        "<p>This is an MSX serving files over HTTP. "
        "Try <a href=\"/_list\">/_list</a> for the file listing.</p>"
        "</body></html>\n";
    u16 bodyLen = sizeof(body) - 1;
    HttpSendStr(conn, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: ");
    HttpSendU32(conn, (u32)bodyLen);
    HttpSendStr(conn, "\r\nConnection: close\r\n\r\n");
    Net_Send(conn, (const u8*)body, bodyLen);
}

// ── GET /_list — listado con paginacion ──
// Hace dos pasadas por FFIRST/FNEXT:
//   1ª: cuenta total (X-Total-Count)
//   2ª: salta `from`, emite hasta `limit` lineas "name<TAB>size<LF>"
// Como Content-Length se debe enviar antes del body, primero construimos
// el body en g_HttpResp y luego mandamos cabecera + cuerpo de una.
// Para 256 entradas con 8.3 nombres maxima longitud = 256 * 17 = 4352 B.
// g_HttpResp es 1024 — insuficiente. Solucion: usamos un buffer mas grande
// reusando g_Files[] como espacio temporal (8 KB en BSS, no en uso durante
// el servicio HTTP porque la UI esta congelada).
static void HttpHandleGetList(NetConn conn, u32 from, u32 limit)
{
    // Buffer de body — reusamos g_Files como area de bytes (8 KB)
    c8* bodyBuf = (c8*)g_Files;
    const u16 bodyMax = sizeof(g_Files);
    u16 bodyLen = 0;

    if(limit == 0) limit = 256;
    if(limit > 256) limit = 256;

    // 1ª pasada: contar total
    u32 total = 0;
    {
        DOS_FIB* fib = DOS_FindFirstEntry("*.*", 0);
        while(fib) {
            if((fib->Attribute & ATTR_FOLDER) == 0 &&
               (fib->Attribute & ATTR_DEVICE) == 0)
            {
                total++;
            }
            fib = DOS_FindNextEntry();
        }
    }

    // 2ª pasada: salta from, emite hasta limit
    {
        DOS_FIB* fib = DOS_FindFirstEntry("*.*", 0);
        u32 seen = 0;
        u32 emitted = 0;
        while(fib && emitted < limit) {
            if((fib->Attribute & ATTR_FOLDER) == 0 &&
               (fib->Attribute & ATTR_DEVICE) == 0)
            {
                if(seen >= from) {
                    // Format "name\tsize\n" en bodyBuf
                    u8 k;
                    for(k = 0; k < 13 && fib->Filename[k] && bodyLen < bodyMax; k++) {
                        bodyBuf[bodyLen++] = fib->Filename[k];
                    }
                    if(bodyLen < bodyMax) bodyBuf[bodyLen++] = '\t';
                    // size as decimal
                    {
                        c8 tmp[12]; u8 n = 0; u32 sz = fib->Size;
                        if(sz == 0) tmp[n++] = '0';
                        else { while(sz) { tmp[n++] = '0' + (u8)(sz % 10); sz /= 10; } }
                        while(n && bodyLen < bodyMax) bodyBuf[bodyLen++] = tmp[--n];
                    }
                    if(bodyLen < bodyMax) bodyBuf[bodyLen++] = '\n';
                    emitted++;
                }
                seen++;
            }
            fib = DOS_FindNextEntry();
        }
    }

    // Enviar headers + body
    HttpSendStr(conn, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ");
    HttpSendU32(conn, (u32)bodyLen);
    HttpSendStr(conn, "\r\nX-Total-Count: ");
    HttpSendU32(conn, total);
    HttpSendStr(conn, "\r\nX-Page-Start: ");
    HttpSendU32(conn, from);
    HttpSendStr(conn, "\r\nConnection: close\r\n\r\n");
    if(bodyLen > 0) {
        Net_Send(conn, (const u8*)bodyBuf, bodyLen);
    }
}

// ── GET /<file> ──
static void HttpHandleGetFile(NetConn conn, const c8* path)
{
    c8 fname[16];
    if(!SafeFilename(path, fname)) {
        HttpSendError(conn, 400, "Bad Request", "bad path\n");
        return;
    }
    // FFIRST para saber tamaño
    DOS_FIB* fib = DOS_FindFirstEntry(fname, 0);
    if(!fib) {
        HttpSendError(conn, 404, "Not Found", "no such file\n");
        return;
    }
    u32 total = fib->Size;
    u32 start = g_HttpReqHasRange ? g_HttpReqRangeStart : 0;
    if(start >= total) {
        HttpSendError(conn, 416, "Range Not Satisfiable", "range past EOF\n");
        return;
    }
    u32 length = total - start;
    u16 status = g_HttpReqHasRange ? 206 : 200;
    const c8* statusText = g_HttpReqHasRange ? "Partial Content" : "OK";

    // Open
    u8 fh = DOS_OpenHandle(fname, O_RDONLY);
    if(fh == 0xFF) {
        HttpSendError(conn, 500, "Server Error", "open failed\n");
        return;
    }
    // Seek si Range
    if(start > 0) {
        // SeekHandle: hace falta — MSXgl lo tiene
        DOS_SeekHandle(fh, (i32)start, SEEK_SET);
    }

    // Headers
    HttpSendStr(conn, "HTTP/1.0 ");
    HttpSendU32(conn, (u32)status);
    HttpSendStr(conn, " ");
    HttpSendStr(conn, statusText);
    HttpSendStr(conn, "\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
    HttpSendU32(conn, length);
    HttpSendStr(conn, "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n");
    if(status == 206) {
        HttpSendStr(conn, "Content-Range: bytes ");
        HttpSendU32(conn, start);
        HttpSendStr(conn, "-");
        HttpSendU32(conn, start + length - 1);
        HttpSendStr(conn, "/");
        HttpSendU32(conn, total);
        HttpSendStr(conn, "\r\n");
    }
    HttpSendStr(conn, "\r\n");

    // Body
    while(length > 0) {
        u16 want = (length > HTTP_BODY_CHUNK) ? HTTP_BODY_CHUNK : (u16)length;
        u16 got = DOS_ReadHandle(fh, g_HttpRxBuf, want);
        if(got == 0) break;
        if(!Net_Send(conn, g_HttpRxBuf, got)) break;
        length -= got;
    }
    DOS_CloseHandle(fh);
}

// ── PUT — recibe body en .PRT y al terminar rename a nombre final ──
static void HttpHandlePut(NetConn conn)
{
    c8 fname[16];
    c8 partname[16];
    u8 fh;
    u32 remaining;
    bool ok;

    if(!g_UploadsOn) {
        HttpSendError(conn, 403, "Forbidden", "uploads disabled on this server\n");
        return;
    }
    if(!SafeFilename(g_HttpReqPath, fname)) {
        HttpSendError(conn, 400, "Bad Request", "bad path\n");
        return;
    }
    if(g_HttpReqContentLen == 0) {
        HttpSendError(conn, 411, "Length Required", "Content-Length required for PUT\n");
        return;
    }
    if(g_HttpReqContentLen > 16UL * 1024 * 1024) {
        HttpSendError(conn, 413, "Payload Too Large", "max 16 MiB\n");
        return;
    }

    // Politica de colision
    bool exists = (DOS_FindFirstEntry(fname, 0) != 0);
    if(exists && !g_HttpReqForceOverwrite) {
        HttpSendError(conn, 409, "Conflict",
                      "file exists; use If-Match: * to overwrite\n");
        return;
    }

    // Construye nombre temporal: base + ".PRT" (sustituye extension original)
    {
        u8 k = 0;
        while(fname[k] && fname[k] != '.' && k < 8) {
            partname[k] = fname[k];
            k++;
        }
        partname[k++] = '.';
        partname[k++] = 'P';
        partname[k++] = 'R';
        partname[k++] = 'T';
        partname[k] = 0;
    }
    // Borra .PRT residual si lo hay
    DOS_Delete(partname);

    fh = DOS_CreateHandle(partname, O_WRONLY, 0);
    if(fh == 0xFF) {
        HttpSendError(conn, 500, "Server Error", "create failed on .PRT\n");
        return;
    }

    // Stream body -> .PRT en chunks de HTTP_BODY_CHUNK
    remaining = g_HttpReqContentLen;
    ok = TRUE;
    {
        u16 idle = 0;
        while(remaining > 0) {
            u16 avail = Net_Available(conn);
            if(avail == 0) {
                if(!Net_IsConnected(conn)) { ok = FALSE; break; }
                if(++idle > IDLE_LIMIT)    { ok = FALSE; break; }
                continue;
            }
            idle = 0;
            u16 want = (remaining > HTTP_BODY_CHUNK) ? HTTP_BODY_CHUNK : (u16)remaining;
            if(want > avail) want = avail;
            u16 got = Net_Recv(conn, g_HttpRxBuf, want);
            if(got == 0) { ok = FALSE; break; }
            u16 wrote = DOS_WriteHandle(fh, g_HttpRxBuf, got);
            if(wrote != got) { ok = FALSE; break; }
            remaining -= got;
        }
    }
    DOS_CloseHandle(fh);

    if(!ok) {
        DOS_Delete(partname);
        HttpSendError(conn, 400, "Bad Request", "body truncated or write failed\n");
        return;
    }

    // Borra dest si existe (overwrite o no, ya validamos arriba)
    if(exists) DOS_Delete(fname);

    // Renombra .PRT -> fname. MSXgl solo wrappea HRENAME (0x53) que necesita
    // un handle abierto. Reabrimos el .PRT en lectura, lo renombramos, cerramos.
    u8 rfh = DOS_OpenHandle(partname, O_RDONLY);
    if(rfh == 0xFF) {
        DOS_Delete(partname);
        HttpSendError(conn, 500, "Server Error", "reopen .PRT for rename failed\n");
        return;
    }
    u8 rerr = DOS_RenameHandle(rfh, fname);
    DOS_CloseHandle(rfh);
    if(rerr != 0) {
        DOS_Delete(partname);
        HttpSendError(conn, 500, "Server Error", "rename failed\n");
        return;
    }

    // 201 Created si nuevo, 200 OK si sobreescribio
    u16 status = exists ? 200 : 201;
    const c8* statusText = exists ? "OK" : "Created";
    HttpSendStr(conn, "HTTP/1.0 ");
    HttpSendU32(conn, (u32)status);
    HttpSendStr(conn, " ");
    HttpSendStr(conn, statusText);
    HttpSendStr(conn, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    {
        // body = path + "\n"
        u8 nlen = 0;
        while(g_HttpReqPath[nlen]) nlen++;
        HttpSendU32(conn, (u32)(nlen + 1));
    }
    HttpSendStr(conn, "\r\nConnection: close\r\n\r\n");
    HttpSendStr(conn, g_HttpReqPath);
    HttpSendStr(conn, "\n");
}

// ── dispatcher de una peticion completa ──
static void HttpServeOne(NetConn conn)
{
    if(!HttpReadHeaders(conn)) {
        HttpSendError(conn, 400, "Bad Request", "header read failed\n");
        return;
    }
    if(!HttpParseRequest()) {
        HttpSendError(conn, 400, "Bad Request", "malformed request\n");
        return;
    }

    // ── reflejar en UI ──
    Scr_Locate(0, STATUS_ROW);
    Scr_EraseEOL();
    Scr_PutStr("Serving ");
    Scr_PutStr(g_HttpReqMethod);
    Scr_PutChar(' ');
    {
        u8 k;
        for(k = 0; g_HttpReqPath[k] && k < 60; k++) Scr_PutChar(g_HttpReqPath[k]);
    }

    // Method dispatch
    if(g_HttpReqMethod[0] == 'G' && g_HttpReqMethod[1] == 'E' &&
       g_HttpReqMethod[2] == 'T' && g_HttpReqMethod[3] == 0)
    {
        // Strip query antes de comparar path
        c8 fullPath[80];
        {
            u8 k;
            for(k = 0; g_HttpReqPath[k] && k < 79; k++) fullPath[k] = g_HttpReqPath[k];
            fullPath[k] = 0;
        }
        // Comprobar /_list (con o sin query)
        bool isList = (g_HttpReqPath[0] == '/' && g_HttpReqPath[1] == '_' &&
                       g_HttpReqPath[2] == 'l' && g_HttpReqPath[3] == 'i' &&
                       g_HttpReqPath[4] == 's' && g_HttpReqPath[5] == 't' &&
                       (g_HttpReqPath[6] == 0 || g_HttpReqPath[6] == '?' ||
                        g_HttpReqPath[6] == '/'));
        if(isList) {
            u32 from = 0, limit = 256;
            HttpQueryParamU32("from", &from);
            HttpQueryParamU32("limit", &limit);
            HttpHandleGetList(conn, from, limit);
        } else if(g_HttpReqPath[0] == '/' && g_HttpReqPath[1] == 0) {
            HttpHandleGetRoot(conn);
        } else {
            HttpStripQuery();
            UrlDecodeInPlace(g_HttpReqPath);
            HttpHandleGetFile(conn, g_HttpReqPath);
        }
    }
    else if(g_HttpReqMethod[0] == 'H' && g_HttpReqMethod[1] == 'E' &&
            g_HttpReqMethod[2] == 'A' && g_HttpReqMethod[3] == 'D')
    {
        // Implementacion minima: como GET pero sin body (cerramos antes)
        HttpSendError(conn, 200, "OK", "");
    }
    else if(g_HttpReqMethod[0] == 'P' && g_HttpReqMethod[1] == 'U' &&
            g_HttpReqMethod[2] == 'T' && g_HttpReqMethod[3] == 0)
    {
        HttpHandlePut(conn);
    }
    else {
        HttpSendError(conn, 405, "Method Not Allowed", "use GET/HEAD/PUT\n");
    }

    g_RequestCount++;
}

// ── lifecycle ──
static void Http_Start(void)
{
    if(g_Listener != NET_INVALID_CONN) return;
    g_Listener = Net_OpenPassive(NTS_PORT);
}

static void Http_Stop(void)
{
    if(g_Listener == NET_INVALID_CONN) return;
    Net_Abort(g_Listener);
    g_Listener = NET_INVALID_CONN;
}

static void Http_Tick(void)
{
    if(g_Listener == NET_INVALID_CONN) return;
    u8 r = Net_AcceptIfReady(g_Listener);
    if(r == NET_ACCEPT_READY) {
        NetConn c = g_Listener;
        HttpServeOne(c);
        Net_Close(c);
        g_Listener = NET_INVALID_CONN;
        // Re-arm
        g_Listener = Net_OpenPassive(NTS_PORT);
        // Refresca contador en pantalla
        Scr_Locate(54, STATUS_ROW);
        Scr_PutStr("Requests served: ");
        Scr_PutU32(g_RequestCount);
    } else if(r == NET_ACCEPT_ERROR) {
        Net_Abort(g_Listener);
        g_Listener = Net_OpenPassive(NTS_PORT);
    }
}

//─────────────────────────────────────────────────────────────────
// Main loop
//─────────────────────────────────────────────────────────────────
void main(void)
{
    Scr_Init80();
    Scr_Cls();

    // BSS init manual (MSXgl crt0 no la zerea — ver memorias del proyecto)
    g_FileCount     = 0;
    g_Selection     = 0;
    g_ScrollTop     = 0;
    g_FilterMode    = FILTER_ALL;
    g_FilteredCount = 0;
    g_SharingOn     = FALSE;
    g_UploadsOn     = FALSE;
    g_RequestCount  = 0;
    g_NetReady      = FALSE;
    g_LocalIp[0] = g_LocalIp[1] = g_LocalIp[2] = g_LocalIp[3] = 0;

    // Intenta cargar UNAPI — para mostrar la IP local. Sin UNAPI el server
    // no podra escuchar, pero la UI sigue funcionando (solo lo avisa).
    if(Net_Init()) {
        if(Net_GetLocalIP(g_LocalIp) &&
           (g_LocalIp[0] | g_LocalIp[1] | g_LocalIp[2] | g_LocalIp[3]))
        {
            g_NetReady = TRUE;
        }
    }

    LocalListFill();
    Ui_DrawFrame();

    // Bucle principal — por ahora solo procesa teclas (sin HTTP)
    while(1) {
        u8 row3 = My_Snsmat(3);   // KEY_F (3,3), KEY_D (3,1)
        u8 row4 = My_Snsmat(4);   // KEY_R (4,7), KEY_L (4,1), KEY_O (4,4)
        u8 row5 = My_Snsmat(5);   // KEY_S (5,0), KEY_U (5,2)
        u8 row7 = My_Snsmat(7);
        u8 row8 = My_Snsmat(8);

        if(IS_KEY_PRESSED(row7, KEY_ESC)) {
            WaitKeyRelease();
            break;
        }

        if(IS_KEY_PRESSED(row5, KEY_S)) {
            WaitKeyRelease();
            g_SharingOn = !g_SharingOn;
            if(g_SharingOn) {
                if(!g_NetReady) {
                    // Sin UNAPI no podemos arrancar — revertir el toggle
                    g_SharingOn = FALSE;
                    Scr_Locate(0, STATUS_ROW);
                    Scr_EraseEOL();
                    Scr_PutStr("ERROR: UNAPI not loaded (run UNAPINET first)");
                } else {
                    Http_Start();
                    if(g_Listener == NET_INVALID_CONN) {
                        g_SharingOn = FALSE;
                        Scr_Locate(0, STATUS_ROW);
                        Scr_EraseEOL();
                        Scr_PutStr("ERROR: cannot open passive socket on port 8088");
                    }
                }
            } else {
                Http_Stop();
            }
            Ui_DrawHeader();
            continue;
        }
        if(IS_KEY_PRESSED(row5, KEY_U)) {
            WaitKeyRelease();
            g_UploadsOn = !g_UploadsOn;
            Ui_DrawHeader();
            continue;
        }
        if(IS_KEY_PRESSED(row3, KEY_F)) {
            WaitKeyRelease();
            g_FilterMode = (g_FilterMode + 1) % FILTER_COUNT;
            ApplyFilter();
            Ui_DrawHeader();
            Ui_DrawList();
            continue;
        }
        if(IS_KEY_PRESSED(row4, KEY_R)) {
            WaitKeyRelease();
            LocalListFill();
            Ui_DrawList();
            continue;
        }
        if(IS_KEY_PRESSED(row3, KEY_D)) {
            WaitKeyRelease();
            // TODO: prompt de carpeta — por ahora no implementado
            Scr_Locate(0, STATUS_ROW);
            Scr_EraseEOL();
            Scr_PutStr("(folder picker not implemented yet)");
            continue;
        }

        // UP / DOWN / LEFT / RIGHT
        if(IS_KEY_PRESSED(row8, KEY_UP)) {
            if(g_Selection > 0) {
                MoveSelection(g_Selection - 1);
                { u16 d; for(d = 0; d < 4000; d++) ; }
            }
            continue;
        }
        if(IS_KEY_PRESSED(row8, KEY_DOWN)) {
            if(g_Selection + 1 < g_FilteredCount) {
                MoveSelection(g_Selection + 1);
                { u16 d; for(d = 0; d < 4000; d++) ; }
            }
            continue;
        }
        if(IS_KEY_PRESSED(row8, KEY_LEFT)) {
            if(g_Selection >= ROWS_PER_COL) {
                MoveSelection(g_Selection - ROWS_PER_COL);
                { u16 d; for(d = 0; d < 4000; d++) ; }
            }
            continue;
        }
        if(IS_KEY_PRESSED(row8, KEY_RIGHT)) {
            u8 target = g_Selection + ROWS_PER_COL;
            if(target < g_FilteredCount) {
                MoveSelection(target);
                { u16 d; for(d = 0; d < 4000; d++) ; }
            }
            continue;
        }

        // HTTP server tick — solo cuando sharing esta ON
        if(g_SharingOn) {
            Http_Tick();
        }
    }

    Scr_Restore();
}

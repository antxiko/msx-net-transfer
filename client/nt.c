//=============================================================================
// nt.c — MSX Net Transfer 0.2.2 — cliente HTTP para MSX-DOS 2
//
// Uso:
//     NT <ip>
//
// Conecta al servidor Net Transfer en <ip>:8088, descarga el listado de
// ficheros (endpoint /_list), muestra un navegador en SCREEN 0 80 columnas
// y permite descargar el fichero seleccionado con ENTER.
//
// Teclas:
//     FLECHA ARRIBA / ABAJO   navegar
//     ENTER                   descargar el fichero seleccionado
//     ESC                     salir
//
// Kudos:
//     Inspirado por HGET de ducasp (cliente HTTP UNAPI para MSX). Net Transfer
//     amplia el concepto con un servidor propio (Rust, multiplataforma) y un
//     navegador interactivo en el cliente MSX.
//
// El puerto es fijo (8088, ver NT_PORT mas abajo) — la idea es que el
// usuario solo tenga que recordar la IP del servidor.
//=============================================================================
#include "msxgl.h"
#include "dos.h"
#include "network.h"
#include "bios.h"
#include "bios_mainrom.h"
#include "bios_var.h"
#include "input.h"

#define NT_VERSION       "0.3.1"
#define NT_PORT          8088
#define MAX_FILES        256         // tamaño de pagina, NO cap de carpeta
#define NAME_LEN         28          // 27 + NUL
#define LIST_BUF_SIZE    8192        // pagina (~256 lineas) cabe holgada
#define RX_BUF_SIZE      1024
#define HDR_BUF_SIZE     512
#define IDLE_LIMIT       20000

#define FILTER_ALL       0
#define FILTER_ROM       1
#define FILTER_DSK       2
#define FILTER_COUNT     3

//─────────────────────────────────────────────────────────────────
// Estado global
//─────────────────────────────────────────────────────────────────
static u8  g_Ip[4];
static u16 g_Port = NT_PORT;   // puerto del server (descubierto o NT_PORT por defecto)
static c8  g_HostHdr[24];

typedef struct {
    c8  name[NAME_LEN];
    u32 size;
} FileEntry;

// Modo de vista del navegador. SERVER = ficheros remotos (descarga con ENTER).
// LOCAL = ficheros locales del MSX (subida con ENTER).
#define VIEW_SERVER  0
#define VIEW_LOCAL   1

static FileEntry g_Files[MAX_FILES];
static u16 g_FileCount;             // u16 — MAX_FILES=256 no cabe en u8
static u16 g_Selection;
static u16 g_ScrollTop;
static u8  g_ViewMode;             // VIEW_SERVER / VIEW_LOCAL
static c8  g_ListBuf[LIST_BUF_SIZE];
static u16 g_ListLen;

static u8  g_RxBuf[RX_BUF_SIZE];
static c8  g_HdrBuf[HDR_BUF_SIZE];
static u16 g_HdrLen;
static c8  g_ReqBuf[256];

static u16 g_StatusCode;
static u32 g_ContentLen;
static bool g_HaveContentLen;

static c8  g_CmdLine[129];

// ── Discovery (v0.3) ──
#define NT_DISCOVERY_PORT  8089
#define MAX_SERVERS        8
typedef struct {
    u8  ip[4];
    u16 port;
    c8  name[24];
} ServerInfo;
static ServerInfo g_Servers[MAX_SERVERS];
static u8 g_ServerCount;

static u8  g_FilterMode;
static u16 g_FilteredIdx[MAX_FILES];   // ahora u16 — index puede ser hasta 255
static u16 g_FilteredCount;

// Paginacion — el cliente trabaja con paginas de MAX_FILES entradas y pide
// la siguiente al servidor cuando el cursor cruza el limite. Asi soportamos
// carpetas de cualquier tamaño que MSX-DOS pueda enumerar (3000+ ficheros).
static u16 g_PageStart;            // offset en el listado completo
static u16 g_TotalCount;           // total de ficheros en la carpeta

// Traza interna de diagnostico: sin uso en release. Si en algun debug futuro
// hace falta inspeccionar el flujo de HttpFetch, definir ENABLE_TRACE=1 y
// añadir un dump al final del bloque de error.
#define ENABLE_TRACE 0
#if ENABLE_TRACE
    static c8  g_Trace[64];
    static u8  g_TraceLen;
    #define TRACE(C) do { if(g_TraceLen < sizeof(g_Trace)) g_Trace[g_TraceLen++] = (C); } while(0)
#else
    #define TRACE(C) ((void)0)
#endif

// Layout del listado en SCREEN 0 80x24, en DOS columnas (newspaper style):
//   col 0 → cols  0..37 de pantalla
//   col 1 → cols 40..77 de pantalla
// Cada columna muestra: cursor + nombre (hasta 18 ch) + tamano (10 ch).
// Asi caben 36 ficheros simultaneamente (18 filas x 2 cols).
#define LIST_TOP_ROW     3
#define LIST_BOTTOM_ROW  20
#define ROWS_PER_COL     (LIST_BOTTOM_ROW - LIST_TOP_ROW + 1)
#define COLS_COUNT       2
#define LIST_VISIBLE     (ROWS_PER_COL * COLS_COUNT)
#define COL_WIDTH        40            // anchura de cada columna en pantalla
#define NAME_VISIBLE     18            // chars max del nombre mostrado por columna
#define STATUS_ROW       23

//─────────────────────────────────────────────────────────────────
// Pantalla (SCREEN 0, 80 columnas, via BIOS CHGMOD)
//
// IMPORTANTE: en DOS-2 transient con UNAPINET cargado, el BIOS Main no esta
// garantizado en slot 0 page 0, asi que usamos CALSLT ($001C) con la slot
// de EXPTBL ($FCC1) — patron canonico de Konamiman. Salvamos IX/IY porque
// CALSLT puede modificarlos. El argumento del modo va en A explicito.
//─────────────────────────────────────────────────────────────────
static void Scr_Init80(void) __NAKED
{
__asm
    push ix
    push iy
    ld   a, #80
    ld   (#0xF3AE), a            ; LINL40 = 80
    ld   iy, (#0xFCC0)            ; IYh = EXPTBL = slot del Main BIOS
    ld   ix, #0x005F             ; CHGMOD
    xor  a                       ; A = 0 -> SCREEN 0
    call #0x001C                 ; CALSLT (inter-slot)
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

// Llama a SNSMAT (BIOS, $0141) con A=line via CALSLT. Devuelve la matriz en A.
// SDCC 4.5 con --sdcccall 1 (default) pasa el primer u8 en A y devuelve
// el u8 en A. Asi que A ya contiene 'line' al entrar y SNSMAT devuelve en A
// — no tocamos A ni en entrada ni en salida.
static u8 My_Snsmat(u8 line) __NAKED
{
    (void)line;
__asm
    push ix
    push iy
    ld   iy, (#0xFCC0)            ; IYh = slot Main BIOS
    ld   ix, #0x0141             ; SNSMAT
    call #0x001C                 ; CALSLT — A=line entra, A=matriz sale
    pop  iy
    pop  ix
    ret
__endasm;
}

// Espera N ticks del JIFFY (system timer @ 0xFC9E, 50/60Hz). Independiente
// del reloj de CPU, asi en Turbo R el cursor va a la MISMA velocidad que en
// un MSX 3.58MHz (antes usabamos un for() vacio CPU-dependiente → en TR el
// cursor se desbocaba).
static void Wait_Jiffy(u8 ticks)
{
    u16 t0 = *(volatile u16*)0xFC9E;
    while((u16)(*(volatile u16*)0xFC9E - t0) < (u16)ticks) ;
}

static void Scr_Cls(void)            { DOS_CharOutput(0x0C); }
static void Scr_Locate(u8 x, u8 y)
{
    DOS_CharOutput(0x1B);
    DOS_CharOutput('Y');
    DOS_CharOutput(0x20 + y);
    DOS_CharOutput(0x20 + x);
}
static void Scr_EraseEOL(void)
{
    DOS_CharOutput(0x1B);
    DOS_CharOutput('K');
}
static void Scr_PutChar(c8 c)        { DOS_CharOutput(c); }
static void Scr_PutStr(const c8* s)
{
    while(*s) { DOS_CharOutput(*s++); }
}
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
// Imprime entero u32 alineado a la derecha en un campo de 'width' columnas
static void Scr_PutU32Right(u32 v, u8 width)
{
    c8 tmp[12]; u8 i = 0;
    if(v == 0) tmp[i++] = '0';
    else { while(v) { tmp[i++] = '0' + (u8)(v % 10); v /= 10; } }
    while(i < width) { DOS_CharOutput(' '); width--; }
    while(i) DOS_CharOutput(tmp[--i]);
}
static void Scr_HLine(u8 y, c8 ch)
{
    u8 k;
    Scr_Locate(0, y);
    for(k = 0; k < 80; k++) DOS_CharOutput(ch);
}

//─────────────────────────────────────────────────────────────────
// Parser linea de comandos
//─────────────────────────────────────────────────────────────────
static void CmdLine_Capture(void)
{
    u8 len = *((u8*)0x0080);
    u8 i;
    const c8* src = (const c8*)0x0081;
    if(len > 128) len = 128;
    for(i = 0; i < len; i++) g_CmdLine[i] = src[i];
    g_CmdLine[len] = 0;
}
static c8* NextToken(c8** cursor)
{
    c8* p = *cursor;
    c8* tok;
    if(p == 0) return 0;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == 0) { *cursor = 0; return 0; }
    tok = p;
    while(*p && *p != ' ' && *p != '\t') p++;
    if(*p) { *p = 0; p++; }
    *cursor = p;
    return tok;
}

static bool ParseIPv4(const c8* s, u8* out)
{
    u8 part = 0; u16 acc = 0; bool hasDigit = FALSE;
    while(1) {
        c8 c = *s++;
        if(c >= '0' && c <= '9') {
            acc = acc * 10 + (c - '0');
            if(acc > 255) return FALSE;
            hasDigit = TRUE;
        } else if(c == '.' || c == 0) {
            if(!hasDigit) return FALSE;
            out[part++] = (u8)acc;
            if(c == 0) return (part == 4);
            if(part >= 4) return FALSE;
            acc = 0; hasDigit = FALSE;
        } else {
            return FALSE;
        }
    }
}
static u16 ParseU16(const c8* s)
{
    u16 v = 0;
    while(*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}
static u32 ParseU32(const c8* s)
{
    u32 v = 0;
    while(*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static u8 LowerCaseAt(const c8* s, u8 i)
{
    c8 c = s[i];
    if(c >= 'A' && c <= 'Z') c = c + 32;
    return (u8)c;
}
static bool StartsWithI(const c8* haystack, const c8* prefix)
{
    u8 i = 0;
    while(prefix[i]) {
        if(LowerCaseAt(haystack, i) != (u8)prefix[i]) return FALSE;
        i++;
    }
    return TRUE;
}

static void BuildHostHeader(void)
{
    u8 k, n; u8 i = 0; u8 v;
    for(k = 0; k < 4; k++) {
        c8 b[5]; n = 0; v = g_Ip[k];
        if(v == 0) b[n++] = '0';
        else { while(v) { b[n++] = '0' + (v % 10); v /= 10; } }
        while(n--) g_HostHdr[i++] = b[n];
        if(k < 3) g_HostHdr[i++] = '.';
    }
    if(g_Port != 80) {
        u16 p = g_Port; c8 b[6]; n = 0;
        while(p) { b[n++] = '0' + (p % 10); p /= 10; }
        g_HostHdr[i++] = ':';
        while(n--) g_HostHdr[i++] = b[n];
    }
    g_HostHdr[i] = 0;
}

//─────────────────────────────────────────────────────────────────
// HTTP — capa baja
//─────────────────────────────────────────────────────────────────
static u16 BuildRequest(const c8* path)
{
    u16 i = 0; const c8* s;
    #define APPEND(S) do { s = (S); while(*s) g_ReqBuf[i++] = *s++; } while(0)

    APPEND("GET ");
    { const c8* pp = path; while(*pp) g_ReqBuf[i++] = *pp++; }
    APPEND(" HTTP/1.0\r\n");
    APPEND("Host: ");
    { const c8* hh = g_HostHdr; while(*hh) g_ReqBuf[i++] = *hh++; }
    APPEND("\r\nUser-Agent: NT/" NT_VERSION "\r\n");
    APPEND("Connection: close\r\nAccept: */*\r\n\r\n");
    #undef APPEND
    return i;
}

static bool ParseHeaders(void)
{
    u16 i, line_start;
    g_StatusCode = 0;
    g_ContentLen = 0;
    g_HaveContentLen = FALSE;

    i = 0;
    while(i < g_HdrLen && g_HdrBuf[i] != ' ' && g_HdrBuf[i] != '\r') i++;
    if(i >= g_HdrLen) return FALSE;
    while(i < g_HdrLen && g_HdrBuf[i] == ' ') i++;
    g_StatusCode = ParseU16(&g_HdrBuf[i]);

    while(i < g_HdrLen && g_HdrBuf[i] != '\n') i++;
    i++;
    line_start = i;

    while(line_start < g_HdrLen) {
        i = line_start;
        while(i < g_HdrLen && g_HdrBuf[i] != '\n') i++;
        if(i == line_start || (i == line_start + 1 && g_HdrBuf[line_start] == '\r')) break;
        if(StartsWithI(&g_HdrBuf[line_start], "content-length:")) {
            u16 j = line_start + 15;
            while(j < i && (g_HdrBuf[j] == ' ' || g_HdrBuf[j] == '\t')) j++;
            g_ContentLen = ParseU32(&g_HdrBuf[j]);
            g_HaveContentLen = TRUE;
        }
        // X-Total-Count: <N>  — total de ficheros en la carpeta (paginacion)
        else if(StartsWithI(&g_HdrBuf[line_start], "x-total-count:")) {
            u16 j = line_start + 14;
            while(j < i && (g_HdrBuf[j] == ' ' || g_HdrBuf[j] == '\t')) j++;
            g_TotalCount = (u16)ParseU32(&g_HdrBuf[j]);
        }
        line_start = i + 1;
    }
    return (g_StatusCode == 200);
}

static u16 WaitForData(NetConn conn)
{
    u16 idle = 0;
    while(1) {
        u16 avail = Net_Available(conn);
        if(avail > 0) return avail;
        if(!Net_IsConnected(conn)) return Net_Available(conn);
        if(++idle > IDLE_LIMIT) return 0;
    }
}

static bool AppendHeader(const u8* chunk, u16 chunk_n, u16* out_body_off, u16* out_body_n)
{
    u16 i;
    for(i = 0; i < chunk_n; i++) {
        if(g_HdrLen >= HDR_BUF_SIZE) return FALSE;
        g_HdrBuf[g_HdrLen++] = (c8)chunk[i];
        if(g_HdrLen >= 4 &&
           g_HdrBuf[g_HdrLen-4] == '\r' &&
           g_HdrBuf[g_HdrLen-3] == '\n' &&
           g_HdrBuf[g_HdrLen-2] == '\r' &&
           g_HdrBuf[g_HdrLen-1] == '\n')
        {
            *out_body_off = i + 1;
            *out_body_n   = chunk_n - (i + 1);
            return TRUE;
        }
    }
    *out_body_off = chunk_n;
    *out_body_n   = 0;
    return FALSE;
}

// Estructura de callback para escribir el cuerpo. Si toFile=TRUE escribe en el
// handle DOS; si no, acumula en g_ListBuf hasta LIST_BUF_SIZE-1 y deja '\0'.
typedef struct {
    bool toFile;
    u8   fileHandle;
    u32  written;
    void (*progress)(u32 bytes, u32 total, bool haveTotal);
} HttpSink;

static bool HttpSink_Write(HttpSink* sink, const u8* data, u16 n)
{
    if(sink->toFile) {
        u16 w = DOS_WriteHandle(sink->fileHandle, data, n);
        if(w != n) return FALSE;
    } else {
        u16 k;
        for(k = 0; k < n; k++) {
            if(g_ListLen >= LIST_BUF_SIZE - 1) return FALSE;
            g_ListBuf[g_ListLen++] = (c8)data[k];
        }
    }
    sink->written += n;
    return TRUE;
}

//─────────────────────────────────────────────────────────────────
// HttpFetch: alto nivel. Devuelve TRUE si OK (status 200, todo leido).
//─────────────────────────────────────────────────────────────────
static bool HttpFetch(const c8* path, HttpSink* sink)
{
    NetConn conn;
    bool hdrDone = FALSE;
    bool ok = FALSE;
    u16 reqLen;
    u32 lastProgress = 0;

    g_HdrLen = 0;
    g_ListLen = 0;
    sink->written = 0;
    #if ENABLE_TRACE
    g_TraceLen = 0;
    #endif

    TRACE('O');                       // intento open
    conn = Net_Open(g_Ip, g_Port);
    if(conn == NET_INVALID_CONN) { TRACE('!'); return FALSE; }
    TRACE('o');                       // open OK

    {
        u16 idle = 0;
        TRACE('W');                   // wait connected
        while(!Net_IsConnected(conn)) {
            if(++idle > IDLE_LIMIT) { TRACE('t'); Net_Abort(conn); return FALSE; }
        }
        TRACE('w');                   // connected
    }

    TRACE('B');
    reqLen = BuildRequest(path);
    TRACE('S');                       // antes de send
    {
        u8 sr = Net_Send(conn, (const u8*)g_ReqBuf, reqLen);
        if(!sr) { TRACE('!'); Net_Abort(conn); return FALSE; }
    }
    TRACE('s');                       // send OK
    TRACE('R');                       // entra a recv loop

    while(1) {
        u16 avail = WaitForData(conn);
        u16 want, got;
        if(avail == 0) {
            if(!Net_IsConnected(conn)) { TRACE('D'); ok = hdrDone && (!g_HaveContentLen || sink->written >= g_ContentLen); break; }
            TRACE('T'); break;
        }
        TRACE('a');

        want = avail;
        if(want > RX_BUF_SIZE) want = RX_BUF_SIZE;
        got = Net_Recv(conn, g_RxBuf, want);
        if(got == 0) { TRACE('r'); break; }
        TRACE('A');

        if(!hdrDone) {
            u16 bodyOff, bodyN;
            hdrDone = AppendHeader(g_RxBuf, got, &bodyOff, &bodyN);
            if(hdrDone) {
                TRACE('H');
                if(!ParseHeaders()) { TRACE('p'); break; }
                TRACE('P');
                if(bodyN > 0) {
                    if(!HttpSink_Write(sink, &g_RxBuf[bodyOff], bodyN)) { TRACE('w'); break; }
                    if(sink->progress) sink->progress(sink->written, g_ContentLen, g_HaveContentLen);
                    lastProgress = sink->written;
                }
            } else if(g_HdrLen >= HDR_BUF_SIZE) {
                TRACE('h'); break;
            }
        } else {
            if(!HttpSink_Write(sink, g_RxBuf, got)) break;
            if(sink->progress && (sink->written - lastProgress) >= 2048) {
                sink->progress(sink->written, g_ContentLen, g_HaveContentLen);
                lastProgress = sink->written;
            }
        }

        if(g_HaveContentLen && sink->written >= g_ContentLen) { ok = TRUE; break; }
    }

    if(sink->progress) sink->progress(sink->written, g_ContentLen, g_HaveContentLen);
    Net_Close(conn);

    // Termina g_ListBuf en NUL si era acumulado en memoria
    if(!sink->toFile && g_ListLen < LIST_BUF_SIZE) g_ListBuf[g_ListLen] = 0;

    return ok;
}

//─────────────────────────────────────────────────────────────────
// Parser del listado: "name\tsize\n" -> g_Files[]
//─────────────────────────────────────────────────────────────────
static void ParseList(void)
{
    u16 i = 0;
    g_FileCount = 0;
    while(i < g_ListLen && g_FileCount < MAX_FILES) {
        u16 nameStart = i;
        u8  k = 0;
        // copia hasta tab o newline
        while(i < g_ListLen && g_ListBuf[i] != '\t' && g_ListBuf[i] != '\n' && g_ListBuf[i] != '\r') {
            if(k < NAME_LEN - 1) g_Files[g_FileCount].name[k++] = g_ListBuf[i];
            i++;
        }
        g_Files[g_FileCount].name[k] = 0;

        u32 sz = 0;
        if(i < g_ListLen && g_ListBuf[i] == '\t') {
            i++;
            while(i < g_ListLen && g_ListBuf[i] >= '0' && g_ListBuf[i] <= '9') {
                sz = sz * 10 + (g_ListBuf[i] - '0');
                i++;
            }
        }
        g_Files[g_FileCount].size = sz;

        // saltar al siguiente registro
        while(i < g_ListLen && g_ListBuf[i] != '\n') i++;
        if(i < g_ListLen) i++;

        if(g_Files[g_FileCount].name[0] != 0) {
            g_FileCount++;
        }
        (void)nameStart;
    }
}

// Forward decls — funciones definidas mas abajo pero usadas aqui:
static void ApplyFilter(void);
static void WaitKeyRelease(void);

//─────────────────────────────────────────────────────────────────
// Construye la URL de listado paginado /_list?from=N&limit=M
// (con limite = MAX_FILES). El buffer es estatico para evitar reservas.
//─────────────────────────────────────────────────────────────────
static c8 g_ListUrl[40];
static const c8* BuildListUrl(void)
{
    u8 i = 0;
    const c8* s = "/_list?from=";
    while(*s) g_ListUrl[i++] = *s++;
    {
        c8 tmp[6]; u8 n = 0; u16 v = g_PageStart;
        if(v == 0) tmp[n++] = '0';
        else { while(v) { tmp[n++] = '0' + (u8)(v % 10); v /= 10; } }
        while(n) g_ListUrl[i++] = tmp[--n];
    }
    s = "&limit=";
    while(*s) g_ListUrl[i++] = *s++;
    {
        c8 tmp[6]; u8 n = 0; u16 v = MAX_FILES;
        while(v) { tmp[n++] = '0' + (u8)(v % 10); v /= 10; }
        while(n) g_ListUrl[i++] = tmp[--n];
    }
    g_ListUrl[i] = 0;
    return g_ListUrl;
}

//─────────────────────────────────────────────────────────────────
// Descarga UNA PAGINA del listado del servidor. La pagina viene
// determinada por g_PageStart (que el caller actualiza antes de llamar).
// Devuelve TRUE si OK. Setea g_TotalCount con el X-Total-Count del header.
//─────────────────────────────────────────────────────────────────
static bool RefreshList(void)
{
    HttpSink sink;
    sink.toFile     = FALSE;
    sink.fileHandle = 0;
    sink.written    = 0;
    sink.progress   = 0;
    g_StatusCode    = 0;
    g_HdrLen        = 0;
    g_TotalCount    = 0;
    if(!HttpFetch(BuildListUrl(), &sink)) return FALSE;
    ParseList();
    ApplyFilter();
    return TRUE;
}

//─────────────────────────────────────────────────────────────────
// Enumera los ficheros del directorio actual de MSX-DOS 2 con FFIRST/FNEXT
// y rellena g_Files[]. Pagina segun g_PageStart (salta primeras N entradas)
// y rellena hasta MAX_FILES. Calcula tambien g_TotalCount (cuenta total
// para mostrar "page X/Y of N" en el status).
//─────────────────────────────────────────────────────────────────
static bool LocalListFill(void)
{
    DOS_FIB* fib;
    u16 seen = 0;            // ficheros validos vistos en el barrido
    g_FileCount = 0;

    fib = DOS_FindFirstEntry("*.*", 0);
    while(fib) {
        // Saltar subdirectorios / devices — no entran en el listado
        if((fib->Attribute & ATTR_FOLDER) == 0 &&
           (fib->Attribute & ATTR_DEVICE) == 0)
        {
            // Si caemos dentro de la ventana [g_PageStart, g_PageStart+MAX_FILES) lo guardamos.
            if(seen >= g_PageStart && g_FileCount < MAX_FILES) {
                u8 k;
                for(k = 0; k < NAME_LEN - 1 && fib->Filename[k]; k++) {
                    g_Files[g_FileCount].name[k] = fib->Filename[k];
                }
                g_Files[g_FileCount].name[k] = 0;
                g_Files[g_FileCount].size = fib->Size;
                g_FileCount++;
            }
            seen++;
        }
        fib = DOS_FindNextEntry();
    }
    g_TotalCount = seen;     // total real de la carpeta
    ApplyFilter();
    return TRUE;
}

//─────────────────────────────────────────────────────────────────
// Construye una peticion PUT en g_ReqBuf. Devuelve su longitud.
// Si forceOverwrite=TRUE incluye cabecera "If-Match: *" para que el
// servidor sobreescriba aunque overwrite=false por defecto.
//─────────────────────────────────────────────────────────────────
static u16 BuildPutRequest(const c8* name, u32 size, bool forceOverwrite)
{
    u16 i = 0; const c8* s;
    #define APPEND(S) do { s = (S); while(*s) g_ReqBuf[i++] = *s++; } while(0)

    APPEND("PUT /");
    { const c8* p = name; while(*p) g_ReqBuf[i++] = *p++; }
    APPEND(" HTTP/1.0\r\nHost: ");
    { const c8* h = g_HostHdr; while(*h) g_ReqBuf[i++] = *h++; }
    APPEND("\r\nContent-Length: ");
    {
        c8 tmp[12]; u8 n = 0; u32 v = size;
        if(v == 0) tmp[n++] = '0';
        else { while(v) { tmp[n++] = '0' + (u8)(v % 10); v /= 10; } }
        while(n) g_ReqBuf[i++] = tmp[--n];
    }
    APPEND("\r\nContent-Type: application/octet-stream\r\n");
    if(forceOverwrite) APPEND("If-Match: *\r\n");
    APPEND("User-Agent: NT/" NT_VERSION "\r\nConnection: close\r\n\r\n");
    #undef APPEND
    return i;
}

//─────────────────────────────────────────────────────────────────
// Conversion nombre largo -> "8.3" en mayusculas para FCB / handle
// (truncado conservador para que quepa siempre)
//─────────────────────────────────────────────────────────────────
static void To83Upper(const c8* src, c8* dst)
{
    u8 i, j;
    c8 base[9]; c8 ext[4];
    u8 dotIdx = 0xFF;   // indice del ultimo punto, 0xFF si no hay ninguno
    u8 k;
    for(k = 0; src[k]; k++) { if(src[k] == '.') dotIdx = k; }

    // base: hasta el ultimo punto o max 8 caracteres
    for(i = 0, j = 0; j < 8 && src[i] && (i != dotIdx); i++) {
        c8 c = src[i];
        if(c >= 'a' && c <= 'z') c = c - 32;
        if(c == ' ') continue;
        base[j++] = c;
    }
    base[j] = 0;
    // ext: hasta 3 caracteres tras el punto
    ext[0] = 0;
    if(dotIdx != 0xFF) {
        for(i = 0, j = 0; j < 3 && src[dotIdx + 1 + i]; i++) {
            c8 c = src[dotIdx + 1 + i];
            if(c >= 'a' && c <= 'z') c = c - 32;
            ext[j++] = c;
        }
        ext[j] = 0;
    }
    // combinar
    { u8 kk; for(kk = 0; base[kk]; kk++) *dst++ = base[kk]; }
    if(ext[0]) {
        *dst++ = '.';
        { u8 kk; for(kk = 0; ext[kk]; kk++) *dst++ = ext[kk]; }
    }
    *dst = 0;
}

//─────────────────────────────────────────────────────────────────
// Filtrado de ficheros por extension
//─────────────────────────────────────────────────────────────────
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
    u16 i;
    g_FilteredCount = 0;
    for(i = 0; i < g_FileCount; i++) {
        if(FileMatchesFilter(g_Files[i].name))
            g_FilteredIdx[g_FilteredCount++] = i;
    }
    g_Selection = 0;
    g_ScrollTop = 0;
}

static void Ui_DrawFilterLabel(void)
{
    Scr_Locate(43, 0);   // col 34 = [SERVER]/[LOCAL], col 43 = filter
    if(g_FilterMode == FILTER_ROM)      Scr_PutStr("[ROM]");
    else if(g_FilterMode == FILTER_DSK) Scr_PutStr("[DSK]");
    else                                Scr_PutStr("[ALL]");
}

//─────────────────────────────────────────────────────────────────
// Render de UI
//─────────────────────────────────────────────────────────────────
static void Ui_DrawFrame(void)
{
    Scr_Cls();
    Scr_Locate(0, 0);
    Scr_PutStr("NT v" NT_VERSION " - Net Transfer browser");

    Scr_Locate(34, 0);
    Scr_PutStr(g_ViewMode == VIEW_LOCAL ? "[LOCAL] " : "[SERVER]");
    Ui_DrawFilterLabel();   // col 43

    Scr_Locate(50, 0);
    Scr_PutStr("Server: ");
    Scr_PutIP(g_Ip);
    Scr_PutChar(':');
    Scr_PutU32((u32)g_Port);

    Scr_HLine(1, '-');
    Scr_HLine(21, '-');

    Scr_Locate(0, 22);
    if(g_ViewMode == VIEW_LOCAL) {
        Scr_PutStr("Arrows  ENTER upload   L/S view   R refresh   ESC exit");
    } else {
        Scr_PutStr("Arrows  ENTER download  L/S view  R refresh  F filter  ESC exit");
    }
}

// Calcula (x,y) en pantalla del item idx, segun layout 2-columnas
// (newspaper-style: idx 0..ROWS_PER_COL-1 en col izquierda, etc.).
// Idx es u16 (g_Selection ahora es u16) pero dentro de una pagina el rel
// siempre cabe en u8 porque LIST_VISIBLE = 36.
static void ItemPos(u16 idx, u8* outX, u8* outY)
{
    u8 rel = (u8)(idx - g_ScrollTop);
    u8 col = rel / ROWS_PER_COL;
    u8 row = rel % ROWS_PER_COL;
    *outX = col * COL_WIDTH;
    *outY = LIST_TOP_ROW + row;
}

static void Ui_DrawListItem(u16 idx)
{
    u8 x, y; u8 k;
    FileEntry* fe = &g_Files[g_FilteredIdx[idx]];
    ItemPos(idx, &x, &y);
    Scr_Locate(x, y);
    // Cursor
    if(idx == g_Selection) Scr_PutChar('>'); else Scr_PutChar(' ');
    Scr_PutChar(' ');
    // Nombre, padding a NAME_VISIBLE
    for(k = 0; k < NAME_VISIBLE && fe->name[k]; k++) {
        Scr_PutChar(fe->name[k]);
    }
    while(k < NAME_VISIBLE) { Scr_PutChar(' '); k++; }
    Scr_PutChar(' ');
    // Tamaño (8 chars derecha, sin sufijo — se sobreentiende que son bytes)
    Scr_PutU32Right(fe->size, 8);
    Scr_PutStr("    ");
}

// Actualiza SOLO el caracter de cursor de un item (sin redibujar el resto).
// Evita parpadeo al mover la seleccion.
static void Ui_DrawCursorAt(u16 idx, bool selected)
{
    u8 x, y;
    ItemPos(idx, &x, &y);
    Scr_Locate(x, y);
    Scr_PutChar(selected ? '>' : ' ');
}

// Imprime u16 en exactamente 3 caracteres, alineado a la derecha (padding
// con espacios). Para 0..999. Usado en el contador "File X of Y" sin
// necesidad de EraseEOL.
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

// Reescribe el status: "file N/M  total T" + indicador de pagina si hay
// mas de una. Sin EraseEOL para evitar parpadeo (la linea siempre tiene
// la misma estructura, los digitos se sobreescriben encima).
static void Ui_UpdateCounter(void)
{
    // Numero absoluto del fichero seleccionado en la carpeta entera
    u16 absSel = g_PageStart + g_Selection + 1;

    Scr_Locate(0, STATUS_ROW);
    Scr_EraseEOL();
    Scr_PutStr("File ");
    Scr_PutU32((u32)absSel);
    Scr_PutStr(" of ");
    Scr_PutU32((u32)g_TotalCount);
    // Indicador de pagina si hay mas de una
    if(g_TotalCount > MAX_FILES) {
        u16 page = (g_PageStart / MAX_FILES) + 1;
        u16 pages = (g_TotalCount + MAX_FILES - 1) / MAX_FILES;
        Scr_PutStr("   page ");
        Scr_PutU32((u32)page);
        Scr_PutChar('/');
        Scr_PutU32((u32)pages);
    }
}

static void Ui_DrawList(void)
{
    u16 i;
    u16 end16 = g_ScrollTop + (u16)LIST_VISIBLE;
    u16 end = (end16 > g_FilteredCount) ? g_FilteredCount : end16;

    // Limpia las filas completas de pantalla (ambas columnas)
    for(i = LIST_TOP_ROW; i <= LIST_BOTTOM_ROW; i++) {
        Scr_Locate(0, i);
        Scr_EraseEOL();
    }
    // Dibuja los items visibles
    for(i = g_ScrollTop; i < end; i++) Ui_DrawListItem(i);

    // Header (col izquierda + col derecha)
    Scr_Locate(0, 2);
    Scr_EraseEOL();
    Scr_PutStr("  FILE                  SIZE             FILE                  SIZE");

    // Contador (delega en Ui_UpdateCounter para la version paginada)
    if(g_FilteredCount == 0) {
        Scr_Locate(0, STATUS_ROW);
        Scr_EraseEOL();
        Scr_PutStr(g_FileCount == 0 ? "(folder empty)" : "(no files match filter)");
    } else {
        Ui_UpdateCounter();
    }
}

static void Ui_StatusClear(void)
{
    Scr_Locate(0, STATUS_ROW);
    Scr_EraseEOL();
}

static void Ui_Status(const c8* msg)
{
    Ui_StatusClear();
    Scr_Locate(0, STATUS_ROW);
    Scr_PutStr(msg);
}

//─────────────────────────────────────────────────────────────────
// Progress callback durante la descarga — solo reescribe el contador.
// El prefijo "Downloading... " se imprime UNA vez en DownloadSelected
// antes de HttpFetch. Aqui solo actualizamos los 8 digitos de bytes
// en una columna fija → sin parpadeo.
//─────────────────────────────────────────────────────────────────

// Imprime u32 en exactamente 8 caracteres alineado a la derecha (padding
// con espacios). Para 0..99_999_999.
static void Scr_PutU32_8(u32 v)
{
    c8 d[8]; u8 i;
    for(i = 0; i < 8; i++) { d[i] = '0' + (u8)(v % 10); v /= 10; }
    // Padding: convertir ceros a la izquierda en espacios
    {
        bool seenDigit = FALSE;
        u8 k;
        for(k = 0; k < 7; k++) {
            u8 idx = 7 - k;       // de mas significativo a menos
            if(d[idx] != '0') seenDigit = TRUE;
            if(!seenDigit)        Scr_PutChar(' ');
            else                  Scr_PutChar(d[idx]);
        }
        Scr_PutChar(d[0]);        // digito de unidades siempre va
    }
}

#define DOWNLOAD_BYTES_COL  15    // posicion del contador (despues de "Downloading... ")

static void DownloadProgress(u32 bytes, u32 total, bool haveTotal)
{
    (void)total; (void)haveTotal;
    Scr_Locate(DOWNLOAD_BYTES_COL, STATUS_ROW);
    Scr_PutU32_8(bytes);
}

//─────────────────────────────────────────────────────────────────
// Descarga del fichero seleccionado
//─────────────────────────────────────────────────────────────────
static bool DownloadSelected(void)
{
    const c8* srcName = g_Files[g_FilteredIdx[g_Selection]].name;
    c8  dstName[16];
    c8  path[32];
    u8  file;
    HttpSink sink;

    // Construye path "/name"
    path[0] = '/';
    {
        u8 i = 0;
        while(srcName[i] && i < 30) { path[i+1] = srcName[i]; i++; }
        path[i+1] = 0;
    }

    To83Upper(srcName, dstName);

    // Si el fichero ya existe en el MSX, preguntar antes de sobreescribir.
    {
        u8 existing = DOS_OpenHandle(dstName, O_RDONLY);
        if(existing != 0xFF) {
            DOS_CloseHandle(existing);
            Ui_Status("File exists! ENTER=overwrite  ESC=skip");
            {
                u8 r7;
                while(1) {
                    r7 = My_Snsmat(7);
                    if(IS_KEY_PRESSED(r7, KEY_ESC)) {
                        WaitKeyRelease();
                        Ui_Status("Skipped.");
                        return FALSE;
                    }
                    if(IS_KEY_PRESSED(r7, KEY_RETURN)) {
                        WaitKeyRelease();
                        break;
                    }
                }
            }
        }
    }

    file = DOS_CreateHandle(dstName, O_WRONLY, 0);
    if(file == 0xFF) {
        Ui_Status("ERROR: cannot create destination file");
        return FALSE;
    }

    // Prefijo de la barra de progreso — se escribe UNA vez. El callback
    // (DownloadProgress) solo reescribe los 8 digitos del contador en la
    // columna DOWNLOAD_BYTES_COL. Sin EraseEOL en cada llamada → sin parpadeo.
    //
    // Layout (fila STATUS_ROW):
    //   cols 0-14:  "Downloading... "
    //   cols 15-22: NNNNNNNN          (contador, 8 chars, se reescribe)
    //   cols 23+:   " bytes  FILENAME" (estatico)
    Ui_StatusClear();
    Scr_Locate(0, STATUS_ROW);
    Scr_PutStr("Downloading... ");
    Scr_PutStr("        ");                  // placeholder de 8 espacios
    Scr_PutStr(" bytes  ");
    Scr_PutStr(dstName);

    sink.toFile     = TRUE;
    sink.fileHandle = file;
    sink.written    = 0;
    sink.progress   = DownloadProgress;

    bool ok = HttpFetch(path, &sink);
    DOS_CloseHandle(file);

    Ui_StatusClear();
    Scr_Locate(0, STATUS_ROW);
    if(ok) {
        Scr_PutStr("OK: ");
        Scr_PutU32(sink.written);
        Scr_PutStr(" bytes -> ");
        Scr_PutStr(dstName);
    } else {
        Scr_PutStr("ERROR: download failed (status=");
        Scr_PutU32((u32)g_StatusCode);
        Scr_PutStr(")");
    }
    return ok;
}

// Forward decl — WaitKeyRelease se define mas abajo pero UploadSelected lo usa.
static void WaitKeyRelease(void);

//─────────────────────────────────────────────────────────────────
// Sube un fichero local al servidor con PUT. Devuelve el status HTTP
// (200, 201, 409, etc.) en exito, o codigos especiales:
//   0 = no se pudo abrir el fichero local
//   1 = error de red
//   2 = abortado por el usuario (ESC durante la subida)
//─────────────────────────────────────────────────────────────────
static u16 DoUpload(const c8* localName, bool forceOverwrite)
{
    DOS_FIB* fib;
    u8  fh;
    u32 fileSize, sent;
    NetConn conn;
    u16 reqLen;

    fib = DOS_FindFirstEntry(localName, 0);
    if(fib == 0) return 0;
    fileSize = fib->Size;

    fh = DOS_OpenHandle(localName, O_RDONLY);
    if(fh == 0xFF) return 0;

    conn = Net_Open(g_Ip, g_Port);
    if(conn == NET_INVALID_CONN) { DOS_CloseHandle(fh); return 1; }

    {
        u16 idle = 0;
        while(!Net_IsConnected(conn)) {
            if(++idle > IDLE_LIMIT) {
                Net_Abort(conn); DOS_CloseHandle(fh); return 1;
            }
        }
    }

    reqLen = BuildPutRequest(localName, fileSize, forceOverwrite);
    if(!Net_Send(conn, (const u8*)g_ReqBuf, reqLen)) {
        Net_Abort(conn); DOS_CloseHandle(fh); return 1;
    }

    // Prefijo del progreso (anchura fija para evitar parpadeo)
    Ui_StatusClear();
    Scr_Locate(0, STATUS_ROW);
    Scr_PutStr("Uploading ");
    Scr_PutStr(localName);
    Scr_PutStr("...  ");
    Scr_Locate(30, STATUS_ROW);
    Scr_PutStr("        ");
    Scr_Locate(38, STATUS_ROW);
    Scr_PutStr(" / ");
    Scr_PutU32_8(fileSize);
    Scr_PutStr(" B");

    sent = 0;
    while(sent < fileSize) {
        u16 want, got;
        u8  row7;
        want = (fileSize - sent > RX_BUF_SIZE) ? RX_BUF_SIZE : (u16)(fileSize - sent);
        got  = DOS_ReadHandle(fh, g_RxBuf, want);
        if(got == 0) { Net_Abort(conn); DOS_CloseHandle(fh); return 1; }
        if(!Net_Send(conn, g_RxBuf, got)) {
            Net_Abort(conn); DOS_CloseHandle(fh); return 1;
        }
        sent += got;

        // Actualiza solo los 8 digitos del contador en col 30
        Scr_Locate(30, STATUS_ROW);
        Scr_PutU32_8(sent);

        // ESC abort
        row7 = My_Snsmat(7);
        if(IS_KEY_PRESSED(row7, KEY_ESC)) {
            Net_Abort(conn); DOS_CloseHandle(fh); return 2;
        }
    }
    DOS_CloseHandle(fh);

    // Lee respuesta — esperamos cabeceras + opcional body corto
    g_HdrLen     = 0;
    g_StatusCode = 0;
    {
        bool hdrDone = FALSE;
        u16  idle = 0;
        while(!hdrDone) {
            u16 avail = Net_Available(conn);
            if(avail == 0) {
                if(!Net_IsConnected(conn)) break;
                if(++idle > IDLE_LIMIT) break;
                continue;
            }
            idle = 0;
            {
                u16 take = (avail > RX_BUF_SIZE) ? RX_BUF_SIZE : avail;
                u16 got  = Net_Recv(conn, g_RxBuf, take);
                u16 bodyOff, bodyN;
                if(got == 0) break;
                hdrDone = AppendHeader(g_RxBuf, got, &bodyOff, &bodyN);
                if(hdrDone) { ParseHeaders(); break; }
                if(g_HdrLen >= HDR_BUF_SIZE) break;
            }
        }
    }
    Net_Close(conn);

    return g_StatusCode == 0 ? 1 : g_StatusCode;
}

//─────────────────────────────────────────────────────────────────
// Despues de subir, muestra el resultado y pregunta si hay 409 si
// se quiere sobreescribir (O) o cancelar (C). Devuelve TRUE si quedo
// finalmente OK (200/201), FALSE si fallo o se cancelo.
//─────────────────────────────────────────────────────────────────
static bool UploadSelected(void)
{
    const c8* name = g_Files[g_FilteredIdx[g_Selection]].name;
    u16 status;

    status = DoUpload(name, FALSE);

    if(status == 200 || status == 201) {
        Ui_StatusClear();
        Scr_Locate(0, STATUS_ROW);
        Scr_PutStr("Upload OK ");
        Scr_PutStr(name);
        return TRUE;
    }

    if(status == 0) { Ui_Status("ERROR: cannot open local file"); return FALSE; }
    if(status == 1) { Ui_Status("ERROR: network failure");        return FALSE; }
    if(status == 2) { Ui_Status("Upload aborted");                return FALSE; }

    if(status == 409) {
        // Conflicto — preguntar al usuario
        Ui_StatusClear();
        Scr_Locate(0, STATUS_ROW);
        Scr_PutStr("File exists. (O)verwrite  (C)ancel ?");
        WaitKeyRelease();
        while(1) {
            u8 row3 = My_Snsmat(3);   // KEY_C
            u8 row4 = My_Snsmat(4);   // KEY_O
            u8 row7 = My_Snsmat(7);   // KEY_ESC
            if(IS_KEY_PRESSED(row4, KEY_O)) {
                WaitKeyRelease();
                status = DoUpload(name, TRUE);
                if(status == 200 || status == 201) {
                    Ui_StatusClear();
                    Scr_Locate(0, STATUS_ROW);
                    Scr_PutStr("Upload OK (overwrite) ");
                    Scr_PutStr(name);
                    return TRUE;
                }
                Ui_StatusClear();
                Scr_Locate(0, STATUS_ROW);
                Scr_PutStr("ERROR: overwrite failed (status=");
                Scr_PutU32((u32)status);
                Scr_PutStr(")");
                return FALSE;
            }
            if(IS_KEY_PRESSED(row3, KEY_C) || IS_KEY_PRESSED(row7, KEY_ESC)) {
                WaitKeyRelease();
                Ui_Status("Upload cancelled");
                return FALSE;
            }
        }
    }

    // Otros codigos
    Ui_StatusClear();
    Scr_Locate(0, STATUS_ROW);
    Scr_PutStr("ERROR: upload failed (status=");
    Scr_PutU32((u32)status);
    Scr_PutStr(")");
    return FALSE;
}

//─────────────────────────────────────────────────────────────────
// Espera a que se suelten todas las teclas (antiboucing)
//─────────────────────────────────────────────────────────────────
static void WaitKeyRelease(void)
{
    u8 row7, row8;
    while(1) {
        row7 = My_Snsmat(7);
        row8 = My_Snsmat(8);
        if((row7 & (KEY_FLAG(KEY_RETURN) | KEY_FLAG(KEY_ESC))) ==
                   (KEY_FLAG(KEY_RETURN) | KEY_FLAG(KEY_ESC)) &&
           (row8 & (KEY_FLAG(KEY_UP) | KEY_FLAG(KEY_DOWN) |
                    KEY_FLAG(KEY_LEFT) | KEY_FLAG(KEY_RIGHT))) ==
                   (KEY_FLAG(KEY_UP) | KEY_FLAG(KEY_DOWN) |
                    KEY_FLAG(KEY_LEFT) | KEY_FLAG(KEY_RIGHT)))
            return;
    }
}

// Muestra "(press ENTER)" y espera a que el usuario lo pulse.
// Usado antes de Scr_Restore() en paths de error para que el mensaje
// no desaparezca al borrar la pantalla el cambio de modo BIOS.
static void WaitEnter(void)
{
    u8 r7;
    Scr_PutStr("(press ENTER)\r\n");
    WaitKeyRelease();
    while(1) {
        r7 = My_Snsmat(7);
        if(IS_KEY_PRESSED(r7, KEY_RETURN)) break;
    }
    WaitKeyRelease();
}

//─────────────────────────────────────────────────────────────────
// Helpers de movimiento del cursor: cambian g_Selection y mantienen
// g_ScrollTop alineado a ROWS_PER_COL (una columna entera a la vez).
//─────────────────────────────────────────────────────────────────
static void MoveSelection(u16 newSel)
{
    u16 prev = g_Selection;
    g_Selection = newSel;

    if(g_Selection < g_ScrollTop) {
        // scroll back por columnas hasta cubrir la nueva seleccion
        while(g_Selection < g_ScrollTop) g_ScrollTop -= ROWS_PER_COL;
        Ui_DrawList();
    } else if(g_Selection >= (g_ScrollTop + LIST_VISIBLE)) {
        while(g_Selection >= (g_ScrollTop + LIST_VISIBLE)) g_ScrollTop += ROWS_PER_COL;
        Ui_DrawList();
    } else {
        Ui_DrawCursorAt(prev, FALSE);
        Ui_DrawCursorAt(g_Selection, TRUE);
        Ui_UpdateCounter();
    }
}

//─────────────────────────────────────────────────────────────────
// Pagina al siguiente bloque de MAX_FILES ficheros. Direction +1/-1.
// Si cursorAtEnd=TRUE, deja el cursor en el ultimo fichero de la pagina
// nueva (util al subir de pagina con UP). Devuelve TRUE si pudo paginar.
//─────────────────────────────────────────────────────────────────
static bool LoadPage(u16 newPageStart, bool cursorAtEnd)
{
    u16 prevPageStart = g_PageStart;
    g_PageStart = newPageStart;

    bool ok;
    if(g_ViewMode == VIEW_LOCAL) {
        LocalListFill();
        ok = TRUE;
    } else {
        ok = RefreshList();
    }
    if(!ok) {
        g_PageStart = prevPageStart;
        return FALSE;
    }
    // ApplyFilter ya hizo Selection=0, ScrollTop=0
    if(cursorAtEnd && g_FilteredCount > 0) {
        g_Selection = g_FilteredCount - 1;
        // Aseguramos que el cursor sea visible
        if(g_Selection >= LIST_VISIBLE) {
            g_ScrollTop = ((g_Selection / ROWS_PER_COL) - (LIST_VISIBLE / ROWS_PER_COL) + 1) * ROWS_PER_COL;
        }
    }
    Ui_DrawList();
    return TRUE;
}

//─────────────────────────────────────────────────────────────────
// Bucle principal del navegador
//─────────────────────────────────────────────────────────────────
static void Browse(void)
{
    Ui_DrawFrame();
    Ui_DrawList();

    while(1) {
        u8 row3 = My_Snsmat(3);     // KEY_F (row 3 bit 3)
        u8 row4 = My_Snsmat(4);     // KEY_R / KEY_L (row 4)
        u8 row5 = My_Snsmat(5);     // KEY_S (row 5)
        u8 row7 = My_Snsmat(7);
        u8 row8 = My_Snsmat(8);

        // ── ESC: salir ──
        if(IS_KEY_PRESSED(row7, KEY_ESC)) {
            WaitKeyRelease();
            return;
        }

        // ── L: cambiar a vista LOCAL (vuelve a pagina 0) ──
        if(IS_KEY_PRESSED(row4, KEY_L) && g_ViewMode != VIEW_LOCAL) {
            WaitKeyRelease();
            g_ViewMode = VIEW_LOCAL;
            g_PageStart = 0;
            LocalListFill();
            Ui_DrawFrame();
            Ui_DrawList();
            continue;
        }

        // ── S: cambiar a vista SERVER (vuelve a pagina 0) ──
        if(IS_KEY_PRESSED(row5, KEY_S) && g_ViewMode != VIEW_SERVER) {
            WaitKeyRelease();
            Ui_StatusClear();
            Scr_Locate(0, STATUS_ROW);
            Scr_PutStr("Loading server list...");
            g_PageStart = 0;
            if(RefreshList()) {
                g_ViewMode = VIEW_SERVER;
                Ui_DrawFrame();
                Ui_DrawList();
            } else {
                Ui_StatusClear();
                Scr_Locate(0, STATUS_ROW);
                Scr_PutStr("ERROR: cannot fetch server list");
            }
            continue;
        }

        // ── R: refrescar listado activo ──
        if(IS_KEY_PRESSED(row4, KEY_R)) {
            WaitKeyRelease();
            Ui_StatusClear();
            Scr_Locate(0, STATUS_ROW);
            Scr_PutStr("Refreshing...");
            if(g_ViewMode == VIEW_LOCAL) {
                LocalListFill();
                Ui_DrawList();
            } else {
                if(RefreshList()) {
                    Ui_DrawList();
                } else {
                    Ui_StatusClear();
                    Scr_Locate(0, STATUS_ROW);
                    Scr_PutStr("ERROR: refresh failed");
                }
            }
            continue;
        }

        // ── F: ciclar filtro ALL → ROM → DSK → ALL ──
        if(IS_KEY_PRESSED(row3, KEY_F)) {
            WaitKeyRelease();
            g_FilterMode = (g_FilterMode + 1) % FILTER_COUNT;
            ApplyFilter();
            Ui_DrawFilterLabel();
            Ui_DrawList();
            continue;
        }

        // ── ENTER: descargar (SERVER) o subir (LOCAL) ──
        if(IS_KEY_PRESSED(row7, KEY_RETURN)) {
            if(g_FilteredCount > 0) {
                WaitKeyRelease();
                if(g_ViewMode == VIEW_SERVER) {
                    DownloadSelected();
                } else {
                    UploadSelected();
                }
                // Espera tecla para acknowledge (sin redibujar la lista al volver)
                Scr_Locate(60, STATUS_ROW);
                Scr_PutStr("  (press a key)");
                {
                    u8 r4, r7, r8;
                    while(1) {
                        r4 = My_Snsmat(4);
                        r7 = My_Snsmat(7);
                        r8 = My_Snsmat(8);
                        if(!IS_KEY_PRESSED(r7, KEY_RETURN) &&
                           !IS_KEY_PRESSED(r7, KEY_ESC) &&
                           !IS_KEY_PRESSED(r4, KEY_R) &&
                           !IS_KEY_PRESSED(r8, KEY_UP)   &&
                           !IS_KEY_PRESSED(r8, KEY_DOWN) &&
                           !IS_KEY_PRESSED(r8, KEY_LEFT) &&
                           !IS_KEY_PRESSED(r8, KEY_RIGHT)) continue;
                        if(IS_KEY_PRESSED(r7, KEY_ESC)) { WaitKeyRelease(); return; }
                        break;
                    }
                }
                WaitKeyRelease();
                // En vez de redibujar la lista entera, solo restauramos status
                // (la lista no ha cambiado) y reposicionamos el cursor.
                Ui_StatusClear();
                Ui_UpdateCounter();
                Ui_DrawCursorAt(g_Selection, TRUE);
            }
            continue;
        }

        // ── UP / DOWN — paginan al cruzar tope; al rebasar las puntas
        //                hacen wrap (cursor en primero -> ultimo absoluto, y al reves)
        if(IS_KEY_PRESSED(row8, KEY_UP)) {
            if(g_Selection > 0) {
                MoveSelection(g_Selection - 1);
            } else if(g_PageStart > 0) {
                u16 newStart = (g_PageStart >= MAX_FILES) ? (g_PageStart - MAX_FILES) : 0;
                LoadPage(newStart, TRUE);
            } else if(g_TotalCount > MAX_FILES) {
                // wrap: saltamos a la ultima pagina, cursor al final
                u16 lastStart = ((g_TotalCount - 1) / MAX_FILES) * MAX_FILES;
                LoadPage(lastStart, TRUE);
            } else if(g_FilteredCount > 0) {
                // wrap dentro de pagina unica
                MoveSelection(g_FilteredCount - 1);
            }
            Wait_Jiffy(3);   // ~17Hz repeat — fixed rate, no Turbo R runaway
            continue;
        }
        if(IS_KEY_PRESSED(row8, KEY_DOWN)) {
            if(g_Selection + 1 < g_FilteredCount) {
                MoveSelection(g_Selection + 1);
            } else if(g_PageStart + g_FileCount < g_TotalCount) {
                LoadPage(g_PageStart + MAX_FILES, FALSE);
            } else if(g_PageStart > 0) {
                // wrap: estamos en la ultima pagina -> volvemos a la primera
                LoadPage(0, FALSE);
            } else if(g_FilteredCount > 0) {
                MoveSelection(0);
            }
            Wait_Jiffy(3);   // ~17Hz repeat — fixed rate, no Turbo R runaway
            continue;
        }

        // ── LEFT / RIGHT: saltar columna. Tambien paginan y hacen wrap igual ──
        if(IS_KEY_PRESSED(row8, KEY_LEFT)) {
            if(g_Selection >= ROWS_PER_COL) {
                MoveSelection(g_Selection - ROWS_PER_COL);
            } else if(g_PageStart > 0) {
                u16 newStart = (g_PageStart >= MAX_FILES) ? (g_PageStart - MAX_FILES) : 0;
                LoadPage(newStart, TRUE);
            } else if(g_TotalCount > MAX_FILES) {
                u16 lastStart = ((g_TotalCount - 1) / MAX_FILES) * MAX_FILES;
                LoadPage(lastStart, TRUE);
            } else if(g_FilteredCount > 0) {
                MoveSelection(g_FilteredCount - 1);
            }
            Wait_Jiffy(3);   // ~17Hz repeat — fixed rate, no Turbo R runaway
            continue;
        }
        if(IS_KEY_PRESSED(row8, KEY_RIGHT)) {
            u16 target = g_Selection + ROWS_PER_COL;
            if(target < g_FilteredCount) {
                MoveSelection(target);
            } else if(g_PageStart + g_FileCount < g_TotalCount) {
                LoadPage(g_PageStart + MAX_FILES, FALSE);
            } else if(g_PageStart > 0) {
                LoadPage(0, FALSE);
            } else if(g_FilteredCount > 0) {
                MoveSelection(0);
            }
            Wait_Jiffy(3);   // ~17Hz repeat — fixed rate, no Turbo R runaway
            continue;
        }
    }
}

//─────────────────────────────────────────────────────────────────
// Print de "uso" — ya en SCREEN 0 80
//─────────────────────────────────────────────────────────────────
static void PrintUsage(void)
{
    Scr_PutStr("NT v" NT_VERSION " - Net Transfer browser\r\n");
    Scr_PutStr("Uso: NT <ip-servidor>\r\n");
    Scr_PutStr("Ej:  NT 192.168.0.102\r\n");
    Scr_PutStr("(Puerto fijo: ");
    Scr_PutU32((u32)g_Port);
    Scr_PutStr(")\r\n");
}

//─────────────────────────────────────────────────────────────────
// Discovery (v0.3) — el MSX solo ESCUCHA. El server PC/MSX anuncia su
// presencia por broadcast a 255.255.255.255:8089 cada ~1s. Nosotros abrimos
// un socket UDP en el 8089 y recolectamos anuncios "NT!" + port + name durante
// una ventana fija, deduplicando por IP origen.
//
// Por que escuchar y no preguntar: recibir un datagrama broadcast NO requiere
// ningun flag especial en la pila UNAPI, asi que funciona en GR8NET, BadCat,
// ESP-FPGA y openMSXnet por igual. ENVIAR broadcast (el modelo inverso) exige
// SO_BROADCAST y no esta garantizado en todos los bridges.
//
// Devuelve numero de servers encontrados (en g_Servers).
//─────────────────────────────────────────────────────────────────
static u8 DiscoverServers(void)
{
    NetConn udp;
    u8 buf[64];
    u16 ticks;
    u16 spin;
    u8 saw_dup;

    g_ServerCount = 0;

    // Escuchamos en el puerto de descubrimiento donde el server anuncia.
    udp = Net_UdpOpen(NT_DISCOVERY_PORT);
    if(udp == NET_INVALID_CONN) return 0;

    // Salimos en cuanto vemos un anuncio DUPLICADO: como cada server anuncia
    // ~1 vez/s, ver un duplicado significa que ya paso un ciclo completo y
    // todos los servers de la LAN han anunciado al menos una vez. El tope de
    // ~5s (saw_dup nunca llega) cubre el caso de que no haya ningun server.
    // Mientras tanto pintamos puntos para que no parezca colgado.
    ticks = 0;
    spin = 0;
    saw_dup = 0;
    while(ticks < 6000 && !saw_dup) {
        ticks++;

        // Feedback visual: un punto cada ~0.3s.
        if(++spin >= 350) {
            spin = 0;
            Scr_PutChar('.');
        }

        if(Net_UdpAvailable(udp) > 0) {
            u16 n = Net_UdpRecv(udp, buf, sizeof(buf));
            if(n >= 6 && buf[0] == 'N' && buf[1] == 'T' && buf[2] == '!') {
                u8 ip[4];
                u16 port = (u16)buf[3] | ((u16)buf[4] << 8);
                u8 dup = 0, j;
                Net_UdpLastSrcIP(ip);
                // Dedup por IP+puerto (los anuncios se repiten cada segundo, y
                // pueden coexistir 2 servers en la misma IP con puertos distintos).
                for(j = 0; j < g_ServerCount; j++) {
                    if(g_Servers[j].ip[0] == ip[0] && g_Servers[j].ip[1] == ip[1] &&
                       g_Servers[j].ip[2] == ip[2] && g_Servers[j].ip[3] == ip[3] &&
                       g_Servers[j].port == port) {
                        dup = 1;
                        break;
                    }
                }
                if(dup) {
                    // Ciclo completo visto → ya tenemos a todos, salimos ya.
                    if(g_ServerCount > 0) saw_dup = 1;
                } else if(g_ServerCount < MAX_SERVERS) {
                    ServerInfo* s = &g_Servers[g_ServerCount];
                    s->ip[0] = ip[0]; s->ip[1] = ip[1];
                    s->ip[2] = ip[2]; s->ip[3] = ip[3];
                    s->port = port;
                    u8 nlen = buf[5];
                    if(nlen > 23) nlen = 23;
                    u8 k;
                    for(k = 0; k < nlen && (6 + k) < n; k++) s->name[k] = (c8)buf[6 + k];
                    s->name[k] = 0;
                    g_ServerCount++;
                }
            }
        }
    }
    Scr_PutStr("\r\n");
    Net_UdpClose(udp);
    return g_ServerCount;
}

// Picker simple: muestra lista numerada, espera tecla 1..MAX_SERVERS o ESC.
// Devuelve indice elegido en g_Servers, o 0xFF si cancela.
static u8 PickServer(void)
{
    if(g_ServerCount == 0) return 0xFF;
    if(g_ServerCount == 1) return 0;

    Scr_Cls();
    Scr_Locate(0, 0);
    Scr_PutStr("NT v" NT_VERSION " - Discovery");
    Scr_HLine(1, '-');

    {
        u8 i;
        for(i = 0; i < g_ServerCount; i++) {
            Scr_Locate(2, 3 + i);
            Scr_PutChar('0' + i + 1);
            Scr_PutStr(") ");
            { u8 k; for(k = 0; g_Servers[i].name[k] && k < 23; k++) Scr_PutChar(g_Servers[i].name[k]); }
            Scr_PutStr("    ");
            Scr_PutIP(g_Servers[i].ip);
            Scr_PutChar(':');
            Scr_PutU32((u32)g_Servers[i].port);
        }
    }
    Scr_Locate(0, 21);
    Scr_HLine(21, '-');
    Scr_Locate(0, 22);
    Scr_PutStr("Pulsa 1-");
    Scr_PutChar('0' + g_ServerCount);
    Scr_PutStr(" para conectar, ESC para cancelar");

    // Wait for digit / ESC
    while(1) {
        u8 row0 = My_Snsmat(0);   // KEY_1..KEY_7
        u8 row1 = My_Snsmat(1);   // KEY_8, KEY_9
        u8 row7 = My_Snsmat(7);   // KEY_ESC
        u8 d;
        if(IS_KEY_PRESSED(row7, KEY_ESC)) { WaitKeyRelease(); return 0xFF; }
        for(d = 1; d <= 7 && d <= g_ServerCount; d++) {
            if(IS_KEY_PRESSED(row0, MAKE_KEY(0, d))) { WaitKeyRelease(); return d - 1; }
        }
        if(g_ServerCount >= 8 && IS_KEY_PRESSED(row1, KEY_8)) { WaitKeyRelease(); return 7; }
    }
}

//─────────────────────────────────────────────────────────────────
// Main — siempre en SCREEN 0 80 columnas
//─────────────────────────────────────────────────────────────────
void main(void)
{
    c8* cursor;
    c8* tok1;
    HttpSink sink;
    bool listOk;

    // PRIMERO: SCREEN 0 80 columnas. Todo el output a partir de aqui sale en
    // modo 80. No volvemos a 40 al salir.
    Scr_Init80();
    Scr_Cls();

    // El crt0 de MSXgl NO zeroea la BSS — solo copia _INITIALIZER -> _INITIALIZED.
    // Asi que las "static u8/u16/..." pueden contener basura. Inicializamos a
    // mano todo lo que esperamos que sea 0.
    g_FileCount      = 0;
    g_Selection      = 0;
    g_ScrollTop      = 0;
    g_PageStart      = 0;
    g_TotalCount     = 0;
    g_ViewMode       = VIEW_SERVER;   // empezamos viendo el servidor
    g_ListLen        = 0;
    g_HdrLen         = 0;
    g_StatusCode     = 0;
    g_ContentLen     = 0;
    g_HaveContentLen = FALSE;
    g_FilterMode    = FILTER_ALL;
    g_FilteredCount = 0;
    #if ENABLE_TRACE
    g_TraceLen       = 0;
    #endif
    g_HostHdr[0]     = 0;
    g_CmdLine[0]     = 0;
    g_ServerCount    = 0;
    g_Port           = NT_PORT;
    // No hace falta zerear los buffers grandes (g_RxBuf, g_ListBuf, g_HdrBuf,
    // g_Files) porque siempre se escriben antes de leerse.

    CmdLine_Capture();
    cursor = g_CmdLine;
    tok1 = NextToken(&cursor);

    // Necesitamos UNAPI tanto para descubrimiento como para HTTP — Net_Init primero.
    if(!Net_Init()) {
        Scr_PutStr("ERROR: no UNAPI TCP/IP. Ejecuta UNAPINET antes.\r\n");
        WaitEnter();
        Scr_Restore();
        return;
    }

    if(tok1 == 0) {
        // ── Sin IP: descubrimiento automatico ──
        Scr_PutStr("Buscando servers Net Transfer en la LAN...\r\n");
        DiscoverServers();
        if(g_ServerCount == 0) {
            Scr_PutStr("No se ha encontrado ningun server.\r\n");
            Scr_PutStr("Arranca el server en el PC (nthttp-gui) o en otro MSX (NTS).\r\n");
            Scr_PutStr("O ejecuta: NT <ip-servidor>\r\n");
            WaitEnter();
            Scr_Restore();
            return;
        }
        // Picker
        u8 chosen = PickServer();
        if(chosen == 0xFF) {
            Scr_Restore();
            return;
        }
        // Set g_Ip y g_Port desde el server seleccionado.
        g_Ip[0] = g_Servers[chosen].ip[0];
        g_Ip[1] = g_Servers[chosen].ip[1];
        g_Ip[2] = g_Servers[chosen].ip[2];
        g_Ip[3] = g_Servers[chosen].ip[3];
        g_Port  = g_Servers[chosen].port;
    } else if(!ParseIPv4(tok1, g_Ip)) {
        Scr_PutStr("ERROR: IP invalida\r\n");
        WaitEnter();
        Scr_Restore();
        return;
    }
    BuildHostHeader();

    Scr_PutStr("Conectando con ");
    Scr_PutIP(g_Ip);
    Scr_PutChar(':');
    Scr_PutU32((u32)g_Port);
    Scr_PutStr(" /_list ...\r\n");

    // Carga primera pagina del listado del servidor (RefreshList construye
    // la URL paginada /_list?from=0&limit=256 y rellena g_TotalCount).
    listOk = RefreshList();
    (void)sink;
    if(!listOk) {
        Scr_PutStr("\r\nERROR: no puedo descargar el listado del servidor.\r\n");
        Scr_PutStr("  - HTTP status: ");
        Scr_PutU32((u32)g_StatusCode);
        Scr_PutStr("\r\n  - Comprueba IP, puerto ");
        Scr_PutU32((u32)g_Port);
        Scr_PutStr(" y que el servidor este corriendo.\r\n");
        WaitEnter();
        Scr_Restore();
        return;
    }

    // Ya estamos en 80 cols — entrar al navegador
    Browse();

    // Volvemos a 40 cols para que DOS muestre el prompt correctamente.
    // (Mientras el navegador corre, el modo es 80; al salir, restauramos.)
    Scr_Restore();
}

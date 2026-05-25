//=============================================================================
// nts.c — MSX Net Transfer 0.3.0-dev — HTTP server for MSX-DOS 2 (skeleton)
//
// SKELETON ONLY — the UI shell is wired up but the HTTP server isn't.
// Toggling S / U / F / D / R updates the on-screen state; no TCP listener
// is opened yet. The full implementation lands in the next iteration.
//
// Architecture and protocol are documented in server-msx/README.md.
//
// Inspired by Konamiman's NestorWeb (especially the passive-listener and
// LetTcpipBreathe patterns from tcpip.c) but no NestorWeb code is included.
//=============================================================================
#include "msxgl.h"
#include "dos.h"
#include "network.h"
#include "bios.h"
#include "bios_mainrom.h"
#include "bios_var.h"
#include "input.h"

#define NTS_VERSION      "0.3.0-dev"
#define NTS_PORT         8088
#define MAX_FILES        64
#define NAME_LEN         28
#define IDLE_LIMIT       20000

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
static u8  g_FileCount;
static u8  g_Selection;
static u8  g_ScrollTop;

static u8  g_FilterMode;
static u8  g_FilteredIdx[MAX_FILES];
static u8  g_FilteredCount;

static u8  g_LocalIp[4];
static bool g_NetReady;        // TRUE si UNAPI esta cargada y hay IP

static bool g_SharingOn;       // toggle S — TCP listener active
static bool g_UploadsOn;       // toggle U — PUT allowed
static u32 g_RequestCount;     // peticiones servidas desde arranque

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
            // TODO: realmente abrir / cerrar el listener pasivo aqui
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

        // TODO: aqui ira tcpip_wait() y el chequeo del listener pasivo.
        // Sin un yield, el bucle quema CPU — para la primera iteracion
        // (sin red) no importa porque solo navegamos.
    }

    Scr_Restore();
}

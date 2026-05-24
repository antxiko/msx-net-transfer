// Build config para NHGET (cliente HTTP NetTransfer)
//
// Genera nethget.com (MSX-DOS 2). Se compila con MSXgl tal cual
// esta en MSXonLIVE/MSXgl. Para construir:
//
//   bash build.sh
//
// (o equivalentemente, desde este directorio:
//   node ../../MSXonLIVE/MSXgl/engine/script/js/build.js
//   — pero usa la copia MSXgl que prefieras)

DoClean   = false;
DoCompile = true;
DoMake    = true;
DoPackage = true;
DoDeploy  = false;     // no copiamos a disco/floppy — lo recoge el usuario
DoRun     = false;

ProjName = "nt";
ProjModules = [ ProjName ];
LibModules  = [ "system", "bios", "memory", "dos" ];
// Path relativo asumiendo que el directorio de build esta en
// <MSXgl>/projects/nethget/ — el build.sh se encarga de copiar esto.
AddSources  = [ "../../engine/src/network/unapi_tcp.asm" ];

Machine = "2";          // MSX2
Target  = "DOS2";       // .COM bajo MSX-DOS 2
DiskSize = "720K";

// El crt0 de MSXgl ya construye argv en 0x100 cuando DOSParseArg=true,
// pero parseamos la linea de comandos a mano desde 0x80 (nuestro propio
// tokenizer). Asi evitamos depender de la convencion de llamada SDCC
// para main(argc, argv). Dejamos DOSParseArg en false.
DOSParseArg = false;

AppSignature = true;
AppCompany   = "AX";
AppID        = "NH";

Verbose          = true;
CompileComplexity = "Default";

// SDCC 4.2.0+ defaults to --sdcccall 1 (register-based args: HL/DE).
// unapi_tcp.asm was written for --sdcccall 0 (stack-based args, SP+4/SP+6).
//
// We do NOT set CompileOpt = "--sdcccall 0" globally because that breaks calls
// to z80.lib arithmetic helpers (__divsint, __mulint...) which are precompiled
// for sdcccall 1 → wrong register/stack layout → crash on any integer division.
//
// Instead, client/network/unapi_tcp.h (a local shadow of the MSXgl original)
// declares every tcpip_* function with __sdcccall(0), so only those call sites
// use the stack-based convention. All other code stays on sdcccall 1.


// Forzamos la RAM (codigo + datos) a empezar en $8000 (pagina 2). Asi nuestras
// estructuras grandes (g_ListBuf, g_Files, g_RxBuf...) NO caen en pagina 1
// ($4000-$7FFF), que es donde UNAPINET conmuta su mapper segment. Sin esto,
// cuando el TSR se paginaba dentro, nuestros datos en pagina 1 quedaban
// ocultos detras de su codigo, y los GET salian con datos basura. Patron
// identico al de tetris.
ForceRamAddr = 0x8000;

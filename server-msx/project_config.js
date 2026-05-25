// Build config para NTS.COM (servidor HTTP NetTransfer en MSX).
// Mismo patron que client/project_config.js, ProjName diferente.

DoClean   = false;
DoCompile = true;
DoMake    = true;
DoPackage = true;
DoDeploy  = false;
DoRun     = false;

ProjName = "nts";
ProjModules = [ ProjName ];
LibModules  = [ "system", "bios", "memory", "dos" ];
// Path relativo asumiendo que el build se hace en <MSXgl>/projects/nts/.
AddSources  = [ "../../engine/src/network/unapi_tcp.asm" ];

Machine = "2";
Target  = "DOS2";
DiskSize = "720K";

// Parser de cmdline lo hacemos a mano leyendo $80 (igual que cliente)
DOSParseArg = false;

AppSignature = true;
AppCompany   = "AX";
AppID        = "NS";   // diferente del cliente (NH) para identificar binario

Verbose          = true;
CompileComplexity = "Default";

// Ver client/project_config.js para por que NO ponemos --sdcccall 0 global
// (z80.lib esta precompilada con sdcccall 1). En su lugar, network/unapi_tcp.h
// declara cada tcpip_* con __sdcccall(0).

// Forzamos RAM en pagina 2 — UNAPINET pagina su segment en pagina 1.
// Igual que el cliente.
ForceRamAddr = 0x8000;

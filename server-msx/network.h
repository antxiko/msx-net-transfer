//=============================================================================
// network.h — Adaptador MSXgl UNAPI TCP (copia del proyecto tetris)
//
// Capa de abstraccion sobre las funciones reales de MSXgl (unapi_tcp.h).
// NO usar inline — SDCC en Z80 corrompe el stack con structs grandes en
// funciones inlineadas. Usar variables globales para los structs UNAPI
// para evitar problemas de stack.
//=============================================================================
#ifndef NETWORK_H
#define NETWORK_H

#include "msxgl.h"
#include "network/unapi_tcp.h"

#define NET_INVALID_CONN    (-1)
typedef int NetConn;

#define NET_OK              1
#define NET_ERROR           0

// Flag de TCP_OPEN para conexion pasiva (listener).
// Konamiman's NestorWeb usa bit 0 (TCP_OPEN_FLAGS_PASSIVE = 1) y el bridge
// openMSXnet/UnapiNet tambien (UnapiNet.cc:642 `passive = flags & 0x01`).
// El nombre CONNTYPE_RESIDENT=1 que define MSXgl es enganoso: en la spec
// real ese bit es passive, no resident. Aqui usamos el valor correcto.
#ifndef CONNTYPE_PASSIVE
#define CONNTYPE_PASSIVE    1
#endif

// Resultados de Net_AcceptIfReady
#define NET_ACCEPT_WAITING  0
#define NET_ACCEPT_READY    1
#define NET_ACCEPT_ERROR    2

static tcpip_unapi_tcp_conn_parms g_TcpParms;
static tcpip_unapi_ip_info        g_IpInfo;
static int                        g_ConnResult = 0;
static u8                         g_NetLastError = 0;
static u8                         g_NetImplCount = 0;

static u8 Net_Init(void)
{
    g_NetImplCount = (u8)tcpip_enumerate();
    return (g_NetImplCount > 0) ? NET_OK : NET_ERROR;
}

static NetConn Net_Open(const u8* ip, u16 port)
{
    int err;
    u8 i;
    u8* p = (u8*)&g_TcpParms;
    for(i = 0; i < sizeof(g_TcpParms); i++) p[i] = 0;

    g_TcpParms.dest_ip[0] = ip[0];
    g_TcpParms.dest_ip[1] = ip[1];
    g_TcpParms.dest_ip[2] = ip[2];
    g_TcpParms.dest_ip[3] = ip[3];
    g_TcpParms.dest_port  = (int)port;
    g_TcpParms.local_port = 0xFFFF;
    g_TcpParms.user_timeout = 0;
    g_TcpParms.flags = CONNTYPE_TRANSIENT;

    g_ConnResult = 0;
    err = tcpip_tcp_open(&g_TcpParms, &g_ConnResult);
    g_NetLastError = (u8)err;
    if(err == ERR_OK) return (NetConn)g_ConnResult;
    return NET_INVALID_CONN;
}

static bool Net_GetLocalIP(u8* ipOut)
{
    if(tcpip_get_ipinfo(&g_IpInfo) != ERR_OK) return FALSE;
    ipOut[0] = g_IpInfo.local_ip[0];
    ipOut[1] = g_IpInfo.local_ip[1];
    ipOut[2] = g_IpInfo.local_ip[2];
    ipOut[3] = g_IpInfo.local_ip[3];
    return (ipOut[0]|ipOut[1]|ipOut[2]|ipOut[3]) ? TRUE : FALSE;
}

static void Net_Close(NetConn conn)   { tcpip_tcp_close((int)conn); }
static void Net_Abort(NetConn conn)   { tcpip_tcp_abort((int)conn); }

static u8 Net_GetConnState(NetConn conn)
{
    int err = tcpip_tcp_state((int)conn, &g_TcpParms);
    g_NetLastError = (u8)err;
    if(err != ERR_OK) return 0xFF;
    return (u8)g_TcpParms.conn_state;
}

static bool Net_IsConnected(NetConn conn)
{
    return (Net_GetConnState(conn) == TCP_STATE_ESTABLISHED) ? TRUE : FALSE;
}

static u8 Net_Send(NetConn conn, const u8* data, u16 length)
{
    int err = tcpip_tcp_send((int)conn, (char*)data, (int)length, 1);
    if(err != ERR_OK) return NET_ERROR;
    tcpip_tcp_flush((int)conn);
    return NET_OK;
}

static u16 Net_Available(NetConn conn)
{
    if(tcpip_tcp_state((int)conn, &g_TcpParms) != ERR_OK) return 0;
    return (u16)g_TcpParms.incoming_bytes;
}

static u16 Net_Recv(NetConn conn, u8* buffer, u16 maxLen)
{
    if(tcpip_tcp_rcv((int)conn, (char*)buffer, (int)maxLen, &g_TcpParms) != ERR_OK)
        return 0;
    return maxLen;
}

static void Net_Flush(NetConn conn) { tcpip_tcp_flush((int)conn); }

//─────────────────────────────────────────────────────────────────
// Passive listener — solo usado por NTS.COM (server MSX)
//
// Modelo: abrimos un socket pasivo y polleamos su estado. UNAPI no
// tiene una operacion accept() separada — el mismo handle pasa de
// LISTEN a ESTABLISHED cuando un cliente conecta. Asi gestionamos una
// sola conexion a la vez (perfectamente aceptable para el use case).
//─────────────────────────────────────────────────────────────────
static NetConn Net_OpenPassive(u16 port)
{
    int err;
    u8 i;
    u8* p = (u8*)&g_TcpParms;
    for(i = 0; i < sizeof(g_TcpParms); i++) p[i] = 0;

    // dest = 0.0.0.0:0 → aceptar de cualquier cliente
    g_TcpParms.dest_ip[0] = 0;
    g_TcpParms.dest_ip[1] = 0;
    g_TcpParms.dest_ip[2] = 0;
    g_TcpParms.dest_ip[3] = 0;
    g_TcpParms.dest_port  = 0;
    g_TcpParms.local_port = (int)port;
    g_TcpParms.user_timeout = 0;
    g_TcpParms.flags = CONNTYPE_PASSIVE;

    g_ConnResult = 0;
    err = tcpip_tcp_open(&g_TcpParms, &g_ConnResult);
    g_NetLastError = (u8)err;
    if(err == ERR_OK) return (NetConn)g_ConnResult;
    return NET_INVALID_CONN;
}

// Devuelve uno de:
//   NET_ACCEPT_WAITING — sigue en LISTEN/SYN_*, no hay cliente todavia
//   NET_ACCEPT_READY   — hay cliente conectado en este mismo handle
//   NET_ACCEPT_ERROR   — la conexion murio, reabrir el listener
static u8 Net_AcceptIfReady(NetConn conn)
{
    u8 st = Net_GetConnState(conn);
    if(st == 0xFF) return NET_ACCEPT_ERROR;
    if(st == TCP_STATE_ESTABLISHED || st == TCP_STATE_CLOSE_WAIT)
        return NET_ACCEPT_READY;
    if(st >= TCP_STATE_FIN_WAIT_1)
        return NET_ACCEPT_ERROR;
    return NET_ACCEPT_WAITING;
}

// Recupera la IP del cliente conectado (4 bytes en ipOut). Solo
// significativo despues de Net_AcceptIfReady → NET_ACCEPT_READY.
static bool Net_GetRemoteIP(NetConn conn, u8* ipOut)
{
    if(tcpip_tcp_state((int)conn, &g_TcpParms) != ERR_OK) return FALSE;
    // Despues de tcpip_tcp_state los primeros 4 bytes de g_TcpParms son la
    // IP remota (dest_ip se sobreescribe con la IP del peer real).
    ipOut[0] = (u8)g_TcpParms.dest_ip[0];
    ipOut[1] = (u8)g_TcpParms.dest_ip[1];
    ipOut[2] = (u8)g_TcpParms.dest_ip[2];
    ipOut[3] = (u8)g_TcpParms.dest_ip[3];
    return TRUE;
}

#endif // NETWORK_H

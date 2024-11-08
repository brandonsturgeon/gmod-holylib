#include "filesystem_base.h" // Has to be before symbols.h
#include "LuaInterface.h"
#include "symbols.h"
#include "detours.h"
#include "module.h"
#include "lua.h"
#include "sourcesdk/hltvserver.h"
#include <unordered_map>
#include "usermessages.h"
#include "sourcesdk/hltvdirector.h"

class CSourceTVLibModule : public IModule
{
public:
	virtual void LuaInit(bool bServerInit) OVERRIDE;
	virtual void LuaShutdown() OVERRIDE;
	virtual void InitDetour(bool bPreServer) OVERRIDE;
	virtual const char* Name() { return "sourcetv"; };
	virtual int Compatibility() { return LINUX32; };
};

static ConVar sourcetv_allownetworking("holylib_sourcetv_allownetworking", "0", 0, "Allows HLTV Clients to send net messages to the server.");
static ConVar sourcetv_allowcommands("holylib_sourcetv_allowcommands", "0", 0, "Allows HLTV Clients to send commands to the server.");

static CSourceTVLibModule g_pSourceTVLibModule;
IModule* pSourceTVLibModule = &g_pSourceTVLibModule;

// NOTE: If in the future, Rubat changes the CHLTVServer class, just get the symbols instead of recreating the functions. 
// Using the original function is in most cases better and easier.
CDemoFile *CHLTVDemoRecorder::GetDemoFile()
{
	return &m_DemoFile;
}

bool CHLTVDemoRecorder::IsRecording()
{
	return m_bIsRecording;
}

bool CHLTVServer::IsTVRelay()
{
	return !IsMasterProxy();
}

int CHLTVServer::GetHLTVSlot()
{
	return m_nPlayerSlot;
}

CHLTVServer* hltv = NULL;
static Detouring::Hook detour_CHLTVServer_CHLTVServer;
static void hook_CHLTVServer_CHLTVServer(CHLTVServer* srv)
{
	hltv = srv;
	detour_CHLTVServer_CHLTVServer.GetTrampoline<Symbols::CHLTVServer_CHLTVServer>()(srv);
}

static Detouring::Hook detour_CHLTVServer_DestroyCHLTVServer;
static void hook_CHLTVServer_DestroyCHLTVServer(CHLTVServer* srv)
{
	hltv = NULL;
	detour_CHLTVServer_DestroyCHLTVServer.GetTrampoline<Symbols::CHLTVServer_DestroyCHLTVServer>()(srv);
}

static int CHLTVClient_TypeID = -1;
PushReferenced_LuaClass(CHLTVClient, CHLTVClient_TypeID)
Get_LuaClass(CHLTVClient, CHLTVClient_TypeID, "HLTVClient")

static Detouring::Hook detour_CHLTVClient_Deconstructor;
static void hook_CHLTVClient_Deconstructor(CHLTVClient* client)
{
	auto it = g_pPushedCHLTVClient.find(client);
	if (it != g_pPushedCHLTVClient.end())
	{
		g_Lua->ReferencePush(it->second);
		g_Lua->SetUserType(-1, NULL);
		g_Lua->Pop(1);
		g_Lua->ReferenceFree(it->second);
		g_pPushedCHLTVClient.erase(it);
	} else {
		if (g_pSourceTVLibModule.InDebug())
		{
			Msg("holylib: Failed to find CHLTVClient in map! (%p)\n", client);
			Msg("Map: ");
			for (auto& [pClient, _] : g_pPushedCHLTVClient)
				Msg("%p ", pClient);
			Msg("\n");
		}
	}

	detour_CHLTVClient_Deconstructor.GetTrampoline<Symbols::CHLTVClient_Deconstructor>()(client);
}

LUA_FUNCTION_STATIC(HLTVClient__tostring)
{
	CHLTVClient* client = Get_CHLTVClient(1, false);
	if (!client || client->IsConnected())
	{
		LUA->PushString("HLTVClient [NULL]");
	} else {
		char szBuf[128] = {};
		V_snprintf(szBuf, sizeof(szBuf),"HLTVClient [%i][%s]", client->GetPlayerSlot(), client->GetClientName()); 
		LUA->PushString(szBuf);
	}

	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient__index)
{
	if (!LUA->FindOnObjectsMetaTable(1, 2))
		LUA->PushNil();

	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_GetSlot)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	LUA->PushNumber(client->GetPlayerSlot());
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_GetUserID)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	LUA->PushNumber(client->GetUserID());
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_GetName)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	LUA->PushString(client->GetClientName());
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_GetSteamID) // Broken -> "STEAM_ID_PENDING"
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	LUA->PushString(client->GetNetworkIDString());
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_Reconnect)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	client->Reconnect();
	return 0;
}

LUA_FUNCTION_STATIC(HLTVClient_ClientPrint)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);

	client->ClientPrintf(LUA->CheckString(2));
	return 0;
}

LUA_FUNCTION_STATIC(HLTVClient_IsValid)
{
	CHLTVClient* client = Get_CHLTVClient(1, false);
	
	LUA->PushBool(client != NULL && client->IsConnected());
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_SendLua)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);
	const char* str = LUA->CheckString(2);
	bool bForceReliable = LUA->GetBool(3);

	// NOTE: Original bug was that we had the wrong bitcount for the net messages type which broke every netmessage we created including this one.
	// It should work now, so let's test it later.
	SVC_UserMessage msg;
	msg.m_nMsgType = Util::pUserMessages->LookupUserMessage("LuaCmd");
	if (msg.m_nMsgType == -1)
	{
		LUA->PushBool(false);
		return 1;
	}

	byte userdata[PAD_NUMBER(MAX_USER_MSG_DATA, 4)];
	msg.m_DataOut.StartWriting(userdata, sizeof(userdata));
	msg.m_DataOut.WriteString(str);

	LUA->PushBool(client->SendNetMsg(msg, bForceReliable));
	return 1;
}

LUA_FUNCTION_STATIC(HLTVClient_FireEvent)
{
	CHLTVClient* client = Get_CHLTVClient(1, true);
	IGameEvent* pEvent = Get_IGameEvent(2, true);

	client->FireGameEvent(pEvent);
	return 0;
}

#define LUA_RECORD_OK 0
#define LUA_RECORD_NOSOURCETV -1
#define LUA_RECORD_NOTMASTER -2
#define LUA_RECORD_ACTIVE -3
#define LUA_RECORD_NOTACTIVE -4
#define LUA_RECORD_INVALIDPATH -5
#define LUA_RECORD_FILEEXISTS -6
LUA_FUNCTION_STATIC(sourcetv_IsActive)
{
	if (hltv)
		LUA->PushBool(hltv->IsActive());
	else
		LUA->PushBool(false);
	
	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_IsRecording)
{
	if (hltv)
		LUA->PushBool(hltv->m_DemoRecorder.IsRecording());
	else
		LUA->PushBool(false);
	
	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_IsMasterProxy)
{
	if (hltv)
		LUA->PushBool(hltv->IsMasterProxy());
	else
		LUA->PushBool(false);
	
	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_IsRelay)
{
	if (hltv)
		LUA->PushBool(hltv->IsTVRelay());
	else
		LUA->PushBool(false);
	
	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_GetClientCount)
{
	if (hltv)
		LUA->PushNumber(hltv->GetClientCount());
	else
		LUA->PushNumber(0);
	
	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_GetHLTVSlot)
{
	if (hltv)
		LUA->PushNumber(hltv->GetHLTVSlot());
	else
		LUA->PushNumber(0);
	
	return 1;
}

static Symbols::COM_IsValidPath func_COM_IsValidPath;
static Symbols::CHLTVDemoRecorder_StartRecording func_CHLTVDemoRecorder_StartRecording;
LUA_FUNCTION_STATIC(sourcetv_StartRecord)
{
	const char* pFileName = LUA->CheckString(1);

	if (!hltv || !hltv->IsActive())
	{
		LUA->PushNumber(LUA_RECORD_NOSOURCETV);
		return 1;
	}

	if (!hltv->IsMasterProxy())
	{
		LUA->PushNumber(LUA_RECORD_NOTMASTER);
		return 1;
	}

	if (hltv->m_DemoRecorder.IsRecording())
	{
		LUA->PushNumber(LUA_RECORD_ACTIVE);
		return 1;
	}

	if (!func_COM_IsValidPath(pFileName))
	{
		LUA->PushNumber(LUA_RECORD_INVALIDPATH);
		return 1;
	}
 
	char name[MAX_OSPATH];
	Q_strncpy(name, pFileName, sizeof(name));
	Q_DefaultExtension(name, ".dem", sizeof(name));

	if (g_pFullFileSystem->FileExists(name))
	{
		LUA->PushNumber(LUA_RECORD_FILEEXISTS);
		return 1;
	}

	if (!func_CHLTVDemoRecorder_StartRecording)
		LUA->ThrowError("Failed to get CHLTVDemoRecorder::StartRecording!");

	func_CHLTVDemoRecorder_StartRecording(&hltv->m_DemoRecorder, name, false);
	LUA->PushNumber(LUA_RECORD_OK);

	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_GetRecordingFile)
{
	if (hltv && hltv->m_DemoRecorder.IsRecording())
		LUA->PushString(hltv->m_DemoRecorder.GetDemoFile()->m_szFileName);
	else
		LUA->PushNil();
	
	return 1;
}

static Symbols::CHLTVDemoRecorder_StopRecording func_CHLTVDemoRecorder_StopRecording;
LUA_FUNCTION_STATIC(sourcetv_StopRecord)
{
	if (!hltv || !hltv->IsActive())
	{
		LUA->PushNumber(LUA_RECORD_NOSOURCETV);
		return 1;
	}

	if (!hltv->IsMasterProxy())
	{
		LUA->PushNumber(LUA_RECORD_NOTMASTER);
		return 1;
	}

	if (!hltv->m_DemoRecorder.IsRecording())
	{
		LUA->PushNumber(LUA_RECORD_NOTACTIVE);
		return 1;
	}

	if (!func_CHLTVDemoRecorder_StopRecording)
		LUA->ThrowError("Failed to get CHLTVDemoRecorder::StopRecording!");

	func_CHLTVDemoRecorder_StopRecording(&hltv->m_DemoRecorder);
	LUA->PushNumber(LUA_RECORD_OK);

	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_GetAll)
{
	LUA->CreateTable();
		if (!hltv || !hltv->IsActive())
			return 1;

		int idx = 0;
		for (int i=0; i< hltv->GetClientCount(); ++i)
		{
			CHLTVClient* client = hltv->Client(i);
			if (!client->IsConnected())
				continue;

			LUA->PushNumber(++idx);
			Push_CHLTVClient(client);
			LUA->RawSet(-3);
		}

	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_GetClient)
{
	if (!hltv || !hltv->IsActive())
		return 0;

	int idx = (int)LUA->CheckNumber(1);
	if (idx >= hltv->GetClientCount())
		return 0;

	CHLTVClient* client = hltv->Client(idx);
	if (client && !client->IsConnected())
		client = NULL;

	Push_CHLTVClient(client);

	return 1;
}

LUA_FUNCTION_STATIC(sourcetv_FireEvent)
{
	if (!hltv || !hltv->IsActive())
		return 0;

	IGameEvent* pEvent = Get_IGameEvent(1, true);
	hltv->FireGameEvent(pEvent);

	return 0;
}

static Detouring::Hook detour_CHLTVClient_ProcessGMod_ClientToServer;
static bool hook_CHLTVClient_ProcessGMod_ClientToServer(CHLTVClient* hltvclient, CLC_GMod_ClientToServer* bf)
{
	VPROF_BUDGET("HolyLib - CHLTVClient::ProcessGMod_ClientToServer", VPROF_BUDGETGROUP_HOLYLIB);

	if (!sourcetv_allownetworking.GetBool())
		return true;

	CModule* module = (CModule*)g_pModuleManager.FindModuleByName("bitbuf");
	if (!module)
	{
		Warning("HolyLib (sourcetv): Failed to find bitbuf module?\n");
		return true;
	}

	if (!module->IsEnabled()) // This relies on the bitbuf module.
		return true;

	bf->m_DataIn.Seek(0);
	int type = bf->m_DataIn.ReadUBitLong(4);
	if (type != 2) // Only handle type 2 -> Lua net message.
		return true;

	bf->m_DataIn.ReadUBitLong(8);
	bf->m_DataIn.ReadUBitLong(22); // Skiping to the header
	//bf->m_DataIn.ReadBitLong(16, false); // The header -> the string. Why not an 12 bits? (This will be read by net.ReadHeader())

	if (Lua::PushHook("HolyLib:OnSourceTVNetMessage")) // Maybe change the name? I don't have a better one rn :/
	{
		Push_CHLTVClient(hltvclient);
		Push_bf_read(&bf->m_DataIn);
		g_Lua->Push(-1);
		int iReference = g_Lua->ReferenceCreate();
		g_Lua->CallFunctionProtected(3, 0, true);
		g_Lua->ReferencePush(iReference);
		g_Lua->SetUserType(-1, NULL); // Make sure that the we don't keep the buffer.
		g_Lua->Pop(1);
		g_Lua->ReferenceFree(iReference);
	}

	return true;
}

static Detouring::Hook detour_CHLTVClient_ExecuteStringCommand;
static bool hook_CHLTVClient_ExecuteStringCommand(CHLTVClient* hltvclient, const char* pCommandString)
{
	VPROF_BUDGET("HolyLib - CHLTVClient::ExecuteStringCommand", VPROF_BUDGETGROUP_HOLYLIB);

	if (!sourcetv_allowcommands.GetBool())
		return detour_CHLTVClient_ExecuteStringCommand.GetTrampoline<Symbols::CHLTVClient_ExecuteStringCommand>()(hltvclient, pCommandString);

	CCommand args;
	if ( !args.Tokenize( pCommandString ) )
		return true;

	if (Lua::PushHook("HolyLib:OnSourceTVCommand")) // Maybe change the name? I don't have a better one rn :/
	{
		Push_CHLTVClient(hltvclient);
		g_Lua->PushString(args[0]); // cmd
		g_Lua->PreCreateTable(args.ArgC(), 0);
			for (int i=1; i<args.ArgC(); ++i) // skip cmd -> 0
			{
				g_Lua->PushNumber(i);
				g_Lua->PushString(args.Arg(i));
				g_Lua->RawSet(-3);
			}
		g_Lua->PushString(args.ArgS());
		if (g_Lua->CallFunctionProtected(5, 1, true))
		{
			bool handled = g_Lua->GetBool(-1); // If true was returned, the command was handled.
			g_Lua->Pop(1);
			if (handled)
				return true;
		}
	}

	// Fallback.
	return detour_CHLTVClient_ExecuteStringCommand.GetTrampoline<Symbols::CHLTVClient_ExecuteStringCommand>()(hltvclient, pCommandString);
}

extern CGlobalVars* gpGlobals;
static Detouring::Hook detour_CHLTVDirector_StartNewShot;
static void hook_CHLTVDirector_StartNewShot(CHLTVDirector* director)
{
	VPROF_BUDGET("HolyLib - CHLTVDirector::StartNewShot", VPROF_BUDGETGROUP_HOLYLIB);

	if (Lua::PushHook("HolyLib:OnSourceTVStartNewShot"))
	{
		if (g_Lua->CallFunctionProtected(1, 1, true))
		{
			bool bCancel = g_Lua->GetBool(-1);
			g_Lua->Pop(1);

			if (bCancel)
			{
				int smallestTick = MAX(0, gpGlobals->tickcount - TIME_TO_TICKS(HLTV_MAX_DELAY));
				director->RemoveEventsFromHistory(smallestTick);

				director->m_nNextShotTick = director->m_nBroadcastTick + TIME_TO_TICKS(MAX_SHOT_LENGTH);

				return;
			}
		}
	}

	detour_CHLTVDirector_StartNewShot.GetTrampoline<Symbols::CHLTVDirector_StartNewShot>()(director);
}

void CSourceTVLibModule::LuaInit(bool bServerInit)
{
	if (bServerInit)
		return;

	CHLTVClient_TypeID = g_Lua->CreateMetaTable("HLTVClient");
		Util::AddFunc(HLTVClient__tostring, "__tostring");
		Util::AddFunc(HLTVClient__index, "__index");
		Util::AddFunc(HLTVClient_GetName, "GetName");
		Util::AddFunc(HLTVClient_GetSlot, "GetSlot");
		Util::AddFunc(HLTVClient_GetSteamID, "GetSteamID");
		Util::AddFunc(HLTVClient_GetUserID, "GetUserID");
		Util::AddFunc(HLTVClient_Reconnect, "Reconnect");
		Util::AddFunc(HLTVClient_IsValid, "IsValid");
		Util::AddFunc(HLTVClient_ClientPrint, "ClientPrint");
		Util::AddFunc(HLTVClient_SendLua, "SendLua");
		Util::AddFunc(HLTVClient_FireEvent, "FireEvent");
	g_Lua->Pop(1);

	Util::StartTable();
		Util::AddFunc(sourcetv_IsActive, "IsActive");
		Util::AddFunc(sourcetv_IsRecording, "IsRecording");
		Util::AddFunc(sourcetv_IsMasterProxy, "IsMasterProxy");
		Util::AddFunc(sourcetv_IsRelay, "IsRelay");
		Util::AddFunc(sourcetv_GetClientCount, "GetClientCount");
		Util::AddFunc(sourcetv_GetHLTVSlot, "GetHLTVSlot");
		Util::AddFunc(sourcetv_StartRecord, "StartRecord");
		Util::AddFunc(sourcetv_GetRecordingFile, "GetRecordingFile");
		Util::AddFunc(sourcetv_StopRecord, "StopRecord");
		Util::AddFunc(sourcetv_FireEvent, "FireEvent");

		// Client Functions
		Util::AddFunc(sourcetv_GetAll, "GetAll");
		Util::AddFunc(sourcetv_GetClient, "GetClient");

		Util::AddValue(LUA_RECORD_OK, "RECORD_OK");

		Util::AddValue(LUA_RECORD_NOSOURCETV, "RECORD_NOSOURCETV");
		Util::AddValue(LUA_RECORD_NOTMASTER, "RECORD_NOTMASTER");
		Util::AddValue(LUA_RECORD_ACTIVE, "RECORD_ACTIVE");
		Util::AddValue(LUA_RECORD_NOTACTIVE, "RECORD_NOTACTIVE");
		Util::AddValue(LUA_RECORD_INVALIDPATH, "RECORD_INVALIDPATH");
		Util::AddValue(LUA_RECORD_FILEEXISTS, "RECORD_FILEEXISTS");
	Util::FinishTable("sourcetv");
}

void CSourceTVLibModule::LuaShutdown()
{
	Util::NukeTable("sourcetv");
	g_pPushedCHLTVClient.clear();
}

void CSourceTVLibModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;

	SourceSDK::ModuleLoader engine_loader("engine");
	Detour::Create(
		&detour_CHLTVClient_ProcessGMod_ClientToServer, "CHLTVClient::ProcessGMod_ClientToServer",
		engine_loader.GetModule(), Symbols::CHLTVClient_ProcessGMod_ClientToServerSym,
		(void*)hook_CHLTVClient_ProcessGMod_ClientToServer, m_pID
	);

	Detour::Create(
		&detour_CHLTVClient_ExecuteStringCommand, "CHLTVClient::ExecuteStringCommand",
		engine_loader.GetModule(), Symbols::CHLTVClient_ExecuteStringCommandSym,
		(void*)hook_CHLTVClient_ExecuteStringCommand, m_pID
	);

	Detour::Create(
		&detour_CHLTVServer_CHLTVServer, "CHLTVServer::CHLTVServer",
		engine_loader.GetModule(), Symbols::CHLTVServer_CHLTVServerSym,
		(void*)hook_CHLTVServer_CHLTVServer, m_pID
	);

	Detour::Create(
		&detour_CHLTVServer_DestroyCHLTVServer, "CHLTVServer::~CHLTVServer",
		engine_loader.GetModule(), Symbols::CHLTVServer_DestroyCHLTVServerSym,
		(void*)hook_CHLTVServer_DestroyCHLTVServer, m_pID
	);

	Detour::Create(
		&detour_CHLTVClient_Deconstructor, "CHLTVClient::~CHLTVClient",
		engine_loader.GetModule(), Symbols::CHLTVClient_DeconstructorSym,
		(void*)hook_CHLTVClient_Deconstructor, m_pID
	);

	SourceSDK::ModuleLoader server_loader("server");
	Detour::Create(
		&detour_CHLTVDirector_StartNewShot, "CHLTVDirector::StartNewShot",
		server_loader.GetModule(), Symbols::CHLTVDirector_StartNewShotSym,
		(void*)hook_CHLTVDirector_StartNewShot, m_pID
	);

	func_CHLTVDemoRecorder_StartRecording = (Symbols::CHLTVDemoRecorder_StartRecording)Detour::GetFunction(engine_loader.GetModule(), Symbols::CHLTVDemoRecorder_StartRecordingSym);
	func_CHLTVDemoRecorder_StopRecording = (Symbols::CHLTVDemoRecorder_StopRecording)Detour::GetFunction(engine_loader.GetModule(), Symbols::CHLTVDemoRecorder_StopRecordingSym);
}
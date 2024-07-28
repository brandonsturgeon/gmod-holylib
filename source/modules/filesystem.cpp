#include "module.h"
#include <GarrysMod/Lua/Interface.h>
#include "lua.h"
#include "filesystem.h"
#include <sourcesdk/filesystem_things.h>
#include <unordered_map>
#include <vprof.h>

class CFileSystemModule : public IModule
{
public:
	virtual void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn);
	virtual void LuaInit(bool bServerInit);
	virtual void LuaShutdown();
	virtual void InitDetour(bool bPreServer);
	virtual void Think(bool bSimulating);
	virtual void Shutdown();
	virtual const char* Name() { return "filesystem"; };
};

static CFileSystemModule g_pFileSystemModule;
IModule* pFileSystemModule = &g_pFileSystemModule;

static ConVar holylib_filesystem_easydircheck("holylib_filesystem_easydircheck", "0", 0, 
	"Checks if the folder CBaseFileSystem::IsDirectory checks has a . in the name after the last /. if so assume it's a file extension.");
static ConVar holylib_filesystem_searchcache("holylib_filesystem_searchcache", "1", 0, 
	"If enabled, it will cache the search path a file was located in and if the same file is requested, it will use that search path directly.");
static ConVar holylib_filesystem_optimizedfixpath("holylib_filesystem_optimizedfixpath", "1", 0, 
	"If enabled, it will optimize CBaseFilesystem::FixUpPath by caching the BASE_PATH search cache.");
static ConVar holylib_filesystem_earlysearchcache("holylib_filesystem_earlysearchcache", "1", 0, 
	"If enabled, it will check early in CBaseFilesystem::OpenForRead if the file is in the search cache.");
static ConVar holylib_filesystem_forcepath("holylib_filesystem_forcepath", "1", 0, 
	"If enabled, it will change the paths of some specific files");
static ConVar holylib_filesystem_predictpath("holylib_filesystem_predictpath", "1", 0, 
	"If enabled, it will try to predict the path of a file");
static ConVar holylib_filesystem_predictexistance("holylib_filesystem_predictexistance", "1", 0, 
	"If enabled, it will try to predict the path of a file, but if the file doesn't exist in the predicted path, we'll just say it doesn't exist.");
static ConVar holylib_filesystem_splitgamepath("holylib_filesystem_splitgamepath", "1", 0, 
	"If enabled, it will create for each content type like models/, materials/ a game path which will be used to find that content.");
static ConVar holylib_filesystem_splitluapath("holylib_filesystem_splitluapath", "0", 0, 
	"If enabled, it will do the same thing holylib_filesystem_splitgamepath does but with lsv. Currently it breaks workshop addons.");
static ConVar holylib_filesystem_splitfallback("holylib_filesystem_splitfallback", "0", 0, 
	"If enabled, it will fallback to the original searchpath if the split path failed.");
static ConVar holylib_filesystem_fixgmodpath("holylib_filesystem_fixgmodpath", "1", 0, 
	"If enabled, it will fix up weird gamemode paths like sandbox/gamemode/sandbox/gamemode which gmod likes to use.");

static ConVar holylib_filesystem_debug("holylib_filesystem_debug", "0", 0, 
	"If enabled, it will show any change to the search cache.");


static const char* nullPath = "NULL_PATH";
static Symbols::CBaseFileSystem_FindSearchPathByStoreId func_CBaseFileSystem_FindSearchPathByStoreId;
static std::unordered_map<std::string, std::unordered_map<std::string, int>> m_SearchCache;
static void AddFileToSearchCache(const char* pFileName, int path, const char* pathID)
{
	if (!pathID)
		pathID = nullPath;

	if (holylib_filesystem_debug.GetBool())
		Msg("Added file %s to seach cache (%i, %s)\n", pFileName, path, pathID);

	m_SearchCache[pathID][pFileName] = path;
}


static void RemoveFileFromSearchCache(const char* pFileName, const char* pathID)
{
	if (!pathID)
		pathID = nullPath;

	if (holylib_filesystem_debug.GetBool())
		Msg("Removed file %s from seach cache! (%s)\n", pFileName, pathID);

	m_SearchCache[pathID].erase(pFileName);
}

static CSearchPath* GetPathFromSearchCache(const char* pFileName, const char* pathID)
{
	if (!pathID)
		pathID = nullPath;

	auto it = m_SearchCache[pathID].find(pFileName);
	if (it == m_SearchCache[pathID].end())
		return NULL;

	if (holylib_filesystem_debug.GetBool())
		Msg("Getting search path for file %s from cache!\n", pFileName);

	return func_CBaseFileSystem_FindSearchPathByStoreId(g_pFullFileSystem, it->second);
}

static void NukeSearchCache() // NOTE: We actually never nuke it :D
{
	if (holylib_filesystem_debug.GetBool())
		Msg("Search cache got nuked\n");

	m_SearchCache.clear();
}

static void DumpSearchcacheCmd(const CCommand &args)
{
	//if (args.ArgC() < 1)
	{
		Msg("---- Search cache ----\n");
		for (auto&[strPath, cache] : m_SearchCache)
		{
			Msg("	\"%s\":\n", strPath.c_str());
			for (auto&[entry, storeID] : cache)
			{
				Msg("		\"%s\": %i\n", entry.c_str(), storeID);
			}
		}
		Msg("---- End of Search cache ----\n");
	//} else {
	//	if (V_stricmp(args.Arg(1), "file") == 0)
	//	{
			// ToDo: Dump it into a file and print the filename, like with vprof.
	//	}
	}
}
static ConCommand dumpsearchcache("holylib_filesystem_dumpsearchcache", DumpSearchcacheCmd, "Dumps the searchcache", 0);

static void GetPathFromIDCmd(const CCommand &args)
{
	if ( args.ArgC() < 1 || V_stricmp(args.Arg(1), "") == 0 )
	{
		Msg("Usage: holylib_filesystem_getpathfromid <id>\n");
		return;
	}

	CSearchPath* path = func_CBaseFileSystem_FindSearchPathByStoreId(g_pFullFileSystem, atoi(args.Arg(1)));
	if (!path)
	{
		Msg("Failed to find CSearchPath :/\n");
		return;
	}

	Msg("Id: &%s\n", args.Arg(1));
	Msg("Path %s\n", path->GetPathString());
}
static ConCommand getpathfromid("holylib_filesystem_getpathfromid", GetPathFromIDCmd, "prints the path of the given searchpath id", 0);

static void NukeSearchcacheCmd(const CCommand &args)
{
	NukeSearchCache();
}
static ConCommand nukesearchcache("holylib_filesystem_nukesearchcache", NukeSearchcacheCmd, "Nukes the searchcache", 0);

static Detouring::Hook detour_CBaseFileSystem_FindFileInSearchPath;
static FileHandle_t* hook_CBaseFileSystem_FindFileInSearchPath(void* filesystem, CFileOpenInfo &openInfo)
{
	if (!holylib_filesystem_searchcache.GetBool())
		return detour_CBaseFileSystem_FindFileInSearchPath.GetTrampoline<Symbols::CBaseFileSystem_FindFileInSearchPath>()(filesystem, openInfo);

	if (!g_pFullFileSystem)
		g_pFullFileSystem = (IFileSystem*)filesystem;

	VPROF_BUDGET("HolyLib - CBaseFileSystem::FindFile", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	CSearchPath* cachePath = GetPathFromSearchCache(openInfo.m_pFileName, openInfo.m_pSearchPath->GetPathIDString());
	if (cachePath)
	{
		VPROF_BUDGET("HolyLib - CBaseFileSystem::FindFile - Cache", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

		const CSearchPath* origPath = openInfo.m_pSearchPath;
		openInfo.m_pSearchPath = cachePath;
		FileHandle_t* file = detour_CBaseFileSystem_FindFileInSearchPath.GetTrampoline<Symbols::CBaseFileSystem_FindFileInSearchPath>()(filesystem, openInfo);
		if (file)
			return file;

		openInfo.m_pSearchPath = origPath;
		RemoveFileFromSearchCache(openInfo.m_pFileName, openInfo.m_pSearchPath->GetPathIDString());
	} else {
		if (holylib_filesystem_debug.GetBool())
			Msg("FindFileInSearchPath: Failed to find cachePath! (%s)\n", openInfo.m_pFileName);
	}

	FileHandle_t* file = detour_CBaseFileSystem_FindFileInSearchPath.GetTrampoline<Symbols::CBaseFileSystem_FindFileInSearchPath>()(filesystem, openInfo);

	if (file)
		AddFileToSearchCache(openInfo.m_pFileName, openInfo.m_pSearchPath->m_storeId, openInfo.m_pSearchPath->GetPathIDString());

	return file;
}

static Detouring::Hook detour_CBaseFileSystem_FastFileTime;
static long hook_CBaseFileSystem_FastFileTime(void* filesystem, const CSearchPath* path, const char* pFileName)
{
	if (!holylib_filesystem_searchcache.GetBool())
		return detour_CBaseFileSystem_FastFileTime.GetTrampoline<Symbols::CBaseFileSystem_FastFileTime>()(filesystem, path, pFileName);

	if (!g_pFullFileSystem)
		g_pFullFileSystem = (IFileSystem*)filesystem;

	VPROF_BUDGET("HolyLib - CBaseFileSystem::FastFileTime", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	CSearchPath* cachePath = GetPathFromSearchCache(pFileName, path->GetPathIDString());
	if (cachePath)
	{
		VPROF_BUDGET("HolyLib - CBaseFileSystem::FastFileTime - Cache", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

		long time = detour_CBaseFileSystem_FastFileTime.GetTrampoline<Symbols::CBaseFileSystem_FastFileTime>()(filesystem, cachePath, pFileName);
		if (time != 0L)
			return time;

		RemoveFileFromSearchCache(pFileName, path->GetPathIDString());
	} else {
		if (holylib_filesystem_debug.GetBool())
			Msg("FastFileTime: Failed to find cachePath! (%s)\n", pFileName);
	}

	long time = detour_CBaseFileSystem_FastFileTime.GetTrampoline<Symbols::CBaseFileSystem_FastFileTime>()(filesystem, path, pFileName);

	if (time != 0L)
		AddFileToSearchCache(pFileName, path->m_storeId, path->GetPathIDString());

	return time;
}

static bool is_file(const char *path) {
	const char *last_slash = strrchr(path, '/');
	const char *last_dot = strrchr(path, '.');

	return last_dot != NULL && (last_slash == NULL || last_dot > last_slash);
}

static Detouring::Hook detour_CBaseFileSystem_IsDirectory;
static bool hook_CBaseFileSystem_IsDirectory(void* filesystem, const char* pFileName, const char* pPathID)
{
	if (holylib_filesystem_easydircheck.GetBool() && is_file(pFileName))
		return false;

	return detour_CBaseFileSystem_IsDirectory.GetTrampoline<Symbols::CBaseFileSystem_IsDirectory>()(filesystem, pFileName, pPathID);
}

static int pBaseLength = 0;
static char pBaseDir[MAX_PATH];
static Detouring::Hook detour_CBaseFileSystem_FixUpPath;
static bool hook_CBaseFileSystem_FixUpPath(IFileSystem* filesystem, const char *pFileName, char *pFixedUpFileName, int sizeFixedUpFileName)
{
	if (!holylib_filesystem_optimizedfixpath.GetBool())
		return detour_CBaseFileSystem_FixUpPath.GetTrampoline<Symbols::CBaseFileSystem_FixUpPath>()(filesystem, pFileName, pFixedUpFileName, sizeFixedUpFileName);

	VPROF_BUDGET("HolyLib - CBaseFileSystem::FixUpPath", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	if (!g_pFullFileSystem)
		g_pFullFileSystem = (IFileSystem*)filesystem;

	V_strncpy( pFixedUpFileName, pFileName, sizeFixedUpFileName );
	V_FixSlashes( pFixedUpFileName, CORRECT_PATH_SEPARATOR );
//	V_RemoveDotSlashes( pFixedUpFileName, CORRECT_PATH_SEPARATOR, true );
	V_FixDoubleSlashes( pFixedUpFileName );

	if ( !V_IsAbsolutePath( pFixedUpFileName ) )
	{
		V_strlower( pFixedUpFileName );
	}
	else 
	{
		// What changed here?
		// Gmod calls GetSearchPath every time FixUpPath is called.
		// Now, we only call it once and then reuse the value, since the BASE_PATH shouldn't change at runtime.

		if ( pBaseLength < 3 )
			pBaseLength = filesystem->GetSearchPath( "BASE_PATH", true, pBaseDir, sizeof( pBaseDir ) );

		if ( pBaseLength )
		{
			if ( *pBaseDir && (pBaseLength+1 < V_strlen( pFixedUpFileName ) ) && (0 != V_strncmp( pBaseDir, pFixedUpFileName, pBaseLength ) )  )
			{
				V_strlower( &pFixedUpFileName[pBaseLength-1] );
			}
		}
		
	}

	return true;
}

static std::string nukeFileExtension(const std::string& fileName) {
	size_t lastDotPos = fileName.find_last_of('.');
	if (lastDotPos == std::string::npos) 
		return fileName;

	return fileName.substr(0, lastDotPos);
}

static std::string getFileExtension(const std::string& fileName) {
	size_t lastDotPos = fileName.find_last_of('.');
	if (lastDotPos == std::string::npos || lastDotPos == fileName.length() - 1)
		return "";

	return fileName.substr(lastDotPos + 1);
}

static const char* GetOverridePath(const char* pFileName, const char* pathID)
{
	if (!holylib_filesystem_splitgamepath.GetBool())
		return NULL;

	std::string strFileName = pFileName;
	if (strFileName.rfind("materials/") == 0)
		return "CONTENT_MATERIALS";

	if (strFileName.rfind("models/") == 0)
		return "CONTENT_MODELS";

	if (strFileName.rfind("sound/") == 0)
		return "CONTENT_SOUNDS";

	if (strFileName.rfind("maps/") == 0)
		return "CONTENT_MAPS";

	if (strFileName.rfind("resource/") == 0)
		return "CONTENT_RESOURCE";

	if (strFileName.rfind("scripts/") == 0)
		return "CONTENT_SCRIPTS";

	if (strFileName.rfind("cfg/") == 0)
		return "CONTENT_CONFIGS";

	if (strFileName.rfind("gamemodes/") == 0)
		return "LUA_GAMEMODES";

	if (strFileName.rfind("lua/includes/") == 0)
		return "LUA_INCLUDES";

	if (pathID && (V_stricmp(pathID, "lsv") == 0 || V_stricmp(pathID, "GAME") == 0) && holylib_filesystem_splitluapath.GetBool())
	{
		if (strFileName.rfind("sandbox/") == 0)
			return "LUA_GAMEMODE_SANDBOX";

		if (strFileName.rfind("effects/") == 0)
			return "LUA_EFFECTS";

		if (strFileName.rfind("entities/") == 0)
			return "LUA_ENTITIES";

		if (strFileName.rfind("weapons/") == 0)
			return "LUA_WEAPONS";

		if (strFileName.rfind("lua/derma/") == 0)
			return "LUA_DERMA";

		if (strFileName.rfind("lua/drive/") == 0)
			return "LUA_DRIVE";

		if (strFileName.rfind("lua/entities/") == 0)
			return "LUA_LUA_ENTITIES";

		if (strFileName.rfind("vgui/") == 0)
			return "LUA_VGUI";

		if (strFileName.rfind("postprocess/") == 0)
			return "LUA_POSTPROCESS";

		if (strFileName.rfind("matproxy/") == 0)
			return "LUA_MATPROXY";

		if (strFileName.rfind("autorun/") == 0)
			return "LUA_AUTORUN";
	}

	return NULL;
}

static std::unordered_map<std::string, std::string> g_pOverridePaths;
static Detouring::Hook detour_CBaseFileSystem_OpenForRead;
static FileHandle_t hook_CBaseFileSystem_OpenForRead(CBaseFileSystem* filesystem, const char *pFileNameT, const char *pOptions, unsigned flags, const char *pathID, char **ppszResolvedFilename)
{
	VPROF_BUDGET("HolyLib - CBaseFileSystem::OpenForRead", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	char pFileNameBuff[MAX_PATH];
	const char *pFileName = pFileNameBuff;

	hook_CBaseFileSystem_FixUpPath(filesystem, pFileNameT, pFileNameBuff, sizeof(pFileNameBuff));

	bool splitPath = false;
	const char* origPath = pathID;
	const char* newPath = GetOverridePath(pFileName, pathID);
	if (newPath)
	{
		if (holylib_filesystem_debug.GetBool())
			Msg("OpenForRead: Found split path! switching (%s, %s)\n", pathID, newPath);

		pathID = newPath;
		splitPath = true;
	}

	if (holylib_filesystem_forcepath.GetBool())
	{
		newPath = NULL;
		std::string strFileName = pFileName;

		auto it = g_pOverridePaths.find(strFileName);
		if (it != g_pOverridePaths.end())
			newPath = it->second.c_str();

		if (newPath)
		{
			FileHandle_t handle = detour_CBaseFileSystem_OpenForRead.GetTrampoline<Symbols::CBaseFileSystem_OpenForRead>()(filesystem, pFileNameT, pOptions, flags, pathID, ppszResolvedFilename);
			if (handle) {
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Found file in forced path! (%s, %s, %s)\n", pFileNameT, pathID, newPath);
				return handle;
			} else {
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Failed to find file in forced path! (%s, %s, %s)\n", pFileNameT, pathID, newPath);
			}
		} else {
			if (holylib_filesystem_debug.GetBool())
				Msg("OpenForRead: File is not in overridePaths (%s, %s)\n", pFileNameT, pathID);
		}
	}

	if (holylib_filesystem_predictpath.GetBool() || holylib_filesystem_predictexistance.GetBool())
	{
		CSearchPath* path = NULL;
		std::string strFileName = pFileNameT;
		std::string extension = getFileExtension(strFileName);

		bool isModel = false;
		if (extension == "vvd" || extension == "vtx" || extension == "phy" || extension == "ani")
			isModel = true;

		if (isModel)
		{
			std::string mdlPath = nukeFileExtension(strFileName);
			if (extension == "vtx")
				mdlPath = nukeFileExtension(mdlPath); // "dx90.vtx" -> "dx90" -> ""

			path = GetPathFromSearchCache((mdlPath + ".mdl").c_str(), pathID);
		}

		if (path)
		{
			CFileOpenInfo openInfo( filesystem, pFileName, NULL, pOptions, flags, ppszResolvedFilename );
			openInfo.m_pSearchPath = path;
			FileHandle_t* file = hook_CBaseFileSystem_FindFileInSearchPath(filesystem, openInfo);
			if (file) {
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Found file in predicted path! (%s, %s)\n", pFileNameT, pathID);
				return file;
			} else {
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Failed to predict file path! (%s, %s)\n", pFileNameT, pathID);

				if (holylib_filesystem_predictexistance.GetBool())
				{
					if (holylib_filesystem_debug.GetBool())
						Msg("OpenForRead: predicted path failed. Let's say it doesn't exist.\n");

					return NULL;
				}
			}
		} else {
			if (holylib_filesystem_debug.GetBool())
				Msg("OpenForRead: Not predicting it! (%s, %s, %s)\n", pFileNameT, pathID, extension.c_str());
		}
	}

	if (!holylib_filesystem_earlysearchcache.GetBool())
		return detour_CBaseFileSystem_OpenForRead.GetTrampoline<Symbols::CBaseFileSystem_OpenForRead>()(filesystem, pFileNameT, pOptions, flags, pathID, ppszResolvedFilename);

	if ( V_IsAbsolutePath( pFileName ) )
		return detour_CBaseFileSystem_OpenForRead.GetTrampoline<Symbols::CBaseFileSystem_OpenForRead>()(filesystem, pFileNameT, pOptions, flags, pathID, ppszResolvedFilename);

	// So we now got the issue that CBaseFileSystem::CSearchPathsIterator::CSearchPathsIterator is way too slow.
	// so we need to skip it. We do this by checking if we got the file in the search cache before we call the OpenForRead function.
	CSearchPath* cachePath = GetPathFromSearchCache(pFileName, pathID);
	if (cachePath)
	{
		VPROF_BUDGET("HolyLib - SearchCache::OpenForRead", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

		CFileOpenInfo openInfo( filesystem, pFileName, NULL, pOptions, flags, ppszResolvedFilename );
		openInfo.m_pSearchPath = cachePath;
		FileHandle_t* file = detour_CBaseFileSystem_FindFileInSearchPath.GetTrampoline<Symbols::CBaseFileSystem_FindFileInSearchPath>()(filesystem, openInfo);
		if (file)
			return file;

		//RemoveFileFromSearchCache(pFileNameT);
	} else {
		if (holylib_filesystem_debug.GetBool())
			Msg("OpenForRead: Failed to find cachePath! (%s)\n", pFileName);
	}
	
	if (splitPath && holylib_filesystem_splitfallback.GetBool())
	{
		FileHandle_t file = detour_CBaseFileSystem_OpenForRead.GetTrampoline<Symbols::CBaseFileSystem_OpenForRead>()(filesystem, pFileNameT, pOptions, flags, pathID, ppszResolvedFilename);
		if (file)
			return file;

		// ToDo: Find out why map content isn't found properly.
		if (holylib_filesystem_debug.GetBool())
			Msg("OpenForRead: Failed to find file in splitPath! Failling back to original. This is slow! (%s)\n", pFileName);

		pathID = origPath;
	}

	return detour_CBaseFileSystem_OpenForRead.GetTrampoline<Symbols::CBaseFileSystem_OpenForRead>()(filesystem, pFileNameT, pOptions, flags, pathID, ppszResolvedFilename);

	/*VPROF("HolyLib - CBaseFileSystem::OpenForRead");

	char pFileNameBuff[MAX_PATH];
	const char *pFileName = pFileNameBuff;

	hook_CBaseFileSystem_FixUpPath(fileSystem, pFileNameT, pFileNameBuff, sizeof(pFileNameBuff));		

	if (!pathID || Q_stricmp( pathID, "GAME" ) == 0)
	{
		CMemoryFileBacking* pBacking = NULL;
		{
			AUTO_LOCK( fileSystem->m_MemoryFileMutex );
			CUtlHashtable< const char*, CMemoryFileBacking* >& table = fileSystem->m_MemoryFileHash;
			UtlHashHandle_t idx = table.Find( pFileName );
			if ( idx != table.InvalidHandle() )
			{
				pBacking = table[idx];
				pBacking->AddRef();
			}
		}
		if ( pBacking )
		{
			if ( pBacking->m_nLength != -1 )
			{
				CFileHandle* pFile = new CMemoryFileHandle( fileSystem, pBacking );
				pFile->m_type = strchr( pOptions, 'b' ) ? FT_MEMORY_BINARY : FT_MEMORY_TEXT;
				return ( FileHandle_t )pFile;
			}
			else
			{
				return ( FileHandle_t )NULL;
			}
		}
	}

	CFileOpenInfo openInfo( fileSystem, pFileName, NULL, pOptions, flags, ppszResolvedFilename );

	if ( V_IsAbsolutePath( pFileName ) )
	{
		openInfo.SetAbsolutePath( "%s", pFileName );

		char *pZipExt = V_stristr( openInfo.m_AbsolutePath, ".zip" CORRECT_PATH_SEPARATOR_S );
		if ( !pZipExt )
			pZipExt = V_stristr( openInfo.m_AbsolutePath, ".bsp" CORRECT_PATH_SEPARATOR_S );
		if ( !pZipExt )
			pZipExt = V_stristr( openInfo.m_AbsolutePath, ".vpk" CORRECT_PATH_SEPARATOR_S );
	
		if ( pZipExt && pZipExt[5] )
		{
			char *pSlash = pZipExt + 4;
			Assert( *pSlash == CORRECT_PATH_SEPARATOR );
			*pSlash = '\0';

			char *pRelativeFileName = pSlash + 1;
			for ( int i = 0; i < fileSystem->m_SearchPaths.Count(); i++ )
			{
				CPackedStore *pVPK = fileSystem->m_SearchPaths[i].GetPackedStore();
				if ( pVPK )
				{
					if ( V_stricmp( pVPK->FullPathName(), openInfo.m_AbsolutePath ) == 0 )
					{
						CPackedStoreFileHandle fHandle = pVPK->OpenFile( pRelativeFileName );
						if ( fHandle )
						{
							openInfo.m_pSearchPath = &m_SearchPaths[i];
							openInfo.SetFromPackedStoredFileHandle( fHandle, this );
						}
						break;
					}
					continue;
				}

				// In .zip?
				CPackFile *pPackFile = fileSystem->m_SearchPaths[i].GetPackFile();
				if ( pPackFile )
				{
					if ( Q_stricmp( pPackFile->m_ZipName.Get(), openInfo.m_AbsolutePath ) == 0 )
					{
						openInfo.m_pSearchPath = &m_SearchPaths[i];
						openInfo.m_pFileHandle = pPackFile->OpenFile( pRelativeFileName, openInfo.m_pOptions );
						openInfo.m_pPackFile = pPackFile;
						break;
					}
				}
			}

			if ( openInfo.m_pFileHandle )
			{
				openInfo.SetResolvedFilename( openInfo.m_pFileName );
				openInfo.HandleFileCRCTracking( pRelativeFileName );
			}
			return (FileHandle_t)openInfo.m_pFileHandle;
		}

		// Otherwise, it must be a regular file, specified by absolute filename
		HandleOpenRegularFile( openInfo, true );

		// !FIXME! We probably need to deal with CRC tracking, right?

		return (FileHandle_t)openInfo.m_pFileHandle;
	}

	// Run through all the search paths.
	PathTypeFilter_t pathFilter = FILTER_NONE;
	if ( IsX360() )
	{
		if ( flags & FSOPEN_NEVERINPACK )
		{
			pathFilter = FILTER_CULLPACK;
		}
		else if ( m_DVDMode == DVDMODE_STRICT )
		{
			// most all files on the dvd are expected to be in the pack
			// don't allow disk paths to be searched, which is very expensive on the dvd
			pathFilter = FILTER_CULLNONPACK;
		}
	}

	const CBaseFileSystem::CSearchPath* cache_path = GetPathFromSearchCache( pFileName, pathID );
	if ( cache_path )
	{
		openInfo.m_pSearchPath = cache_path;
		FileHandle_t filehandle = FindFileInSearchPath( openInfo );
		if ( filehandle )
		{
			if ( !openInfo.m_pSearchPath->m_bIsTrustedForPureServer && openInfo.m_ePureFileClass == ePureServerFileClass_AnyTrusted )
			{
				#ifdef PURE_SERVER_DEBUG_SPEW
					Msg( "Ignoring %s from %s for pure server operation\n", openInfo.m_pFileName, openInfo.m_pSearchPath->GetDebugString() );
				#endif

				m_FileTracker2.NoteFileIgnoredForPureServer( openInfo.m_pFileName, pathID, openInfo.m_pSearchPath->m_storeId );
				Close( filehandle );
				openInfo.m_pFileHandle = NULL;
				if ( ppszResolvedFilename && *ppszResolvedFilename )
				{
					free( *ppszResolvedFilename );
					*ppszResolvedFilename = NULL;
				}
			} else {
				openInfo.HandleFileCRCTracking( openInfo.m_pFileName );
				return filehandle;
			}
		} else {
			RemoveFileFromSearchCache( pFileName, pathID ); // Somehow the file was not found? It probably was deleted
		}
	}

	CSearchPathsIterator iter( fileSystem, &pFileName, pathID, pathFilter );
	for ( CSearchPath* path = iter.GetFirst(); path != NULL; path = iter.GetNext() )
	{
		openInfo.m_pSearchPath = path;
		FileHandle_t filehandle = hook_CBaseFileSystem_FindFileInSearchPath( fileSystem, openInfo );
		if ( filehandle )
		{
			// Check if search path is excluded due to pure server white list,
			// then we should make a note of this fact, and keep searching
			if ( !openInfo.m_pSearchPath->m_bIsTrustedForPureServer && openInfo.m_ePureFileClass == ePureServerFileClass_AnyTrusted )
			{
				#ifdef PURE_SERVER_DEBUG_SPEW
					Msg( "Ignoring %s from %s for pure server operation\n", openInfo.m_pFileName, openInfo.m_pSearchPath->GetDebugString() );
				#endif

				m_FileTracker2.NoteFileIgnoredForPureServer( openInfo.m_pFileName, pathID, openInfo.m_pSearchPath->m_storeId );
				fileSystem->Close( filehandle );
				openInfo.m_pFileHandle = NULL;
				if ( ppszResolvedFilename && *ppszResolvedFilename )
				{
					free( *ppszResolvedFilename );
					*ppszResolvedFilename = NULL;
				}
				continue;
			}

			openInfo.HandleFileCRCTracking( openInfo.m_pFileName );
			return filehandle;
		}
	}

	LogFileOpen( "[Failed]", pFileName, "" );
	return ( FileHandle_t )0;*/
}

/*
 * GMOD first calls GetFileTime and then OpenForRead, so we need to make changes for lua in GetFileTime.
 */

static std::string replaceString(std::string str, const std::string& from, const std::string& to)
{
	size_t startPos = str.find(from);
	if (startPos != std::string::npos)
		str.replace(startPos, from.length(), to);

	return str;
}

/*
 * GMOD Likes to use paths like "sandbox/gamemode/spawnmenu/sandbox/gamemode/spawnmenu/".
 * This wastes performance, so we fix them up to be "sandbox/gamemode/spawnmenu/"
 */
static std::string fixGamemodePath(IFileSystem* filesystem, std::string path)
{
	std::string activeGamemode = filesystem->Gamemodes()->Active().name;
	if (activeGamemode.empty())
		return path;

	std::string searchStr = "/";
	searchStr.append(activeGamemode);
	searchStr.append("/gamemode/"); // Final string should be /[Active Gamemode]/gamemode/
	size_t pos = path.find(searchStr);
	if (pos == std::string::npos)
		return path;

	return path.substr(pos + 1);
}

static Detouring::Hook detour_CBaseFileSystem_GetFileTime;
static long hook_CBaseFileSystem_GetFileTime(IFileSystem* filesystem, const char *pFileName, const char *pPathID)
{
	VPROF_BUDGET("HolyLib - CBaseFileSystem::GetFileTime", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);
	
	const char* origPath = pPathID;
	const char* newPath = GetOverridePath(pFileName, pPathID);
	if (newPath)
	{
		if (holylib_filesystem_debug.GetBool())
			Msg("GetFileTime: Found split path! switching (%s, %s)\n", pPathID, newPath);

		pPathID = newPath;
	}

	std::string strFileName = pFileName; // Workaround for now.
	if (origPath && V_stricmp(origPath, "lsv") == 0 && holylib_filesystem_fixgmodpath.GetBool()) // Some weird things happen in the lsv path.  
	{
		strFileName = fixGamemodePath(filesystem, strFileName);
		strFileName = replaceString(strFileName, "includes/includes/", "includes/"); // What causes this?
	}
	pFileName = strFileName.c_str();

	if (holylib_filesystem_forcepath.GetBool())
	{
		newPath = NULL;

		if (!newPath && strFileName.rfind("gamemodes/base") == 0)
			newPath = "MOD_WRITE";

		if (!newPath && strFileName.rfind("gamemodes/sandbox") == 0)
			newPath = "MOD_WRITE";

		if (!newPath && strFileName.rfind("gamemodes/terrortown") == 0)
			newPath = "MOD_WRITE";

		if (newPath)
		{
			long time = detour_CBaseFileSystem_GetFileTime.GetTrampoline<Symbols::CBaseFileSystem_GetFileTime>()(filesystem, pFileName, pPathID);
			if (time != 0L) {
				if (holylib_filesystem_debug.GetBool())
					Msg("GetFileTime: Found file in forced path! (%s, %s, %s)\n", pFileName, pPathID, newPath);
				return time;
			} else
				if (holylib_filesystem_debug.GetBool())
					Msg("GetFileTime: Failed to find file in forced path! (%s, %s, %s)\n", pFileName, pPathID, newPath);
		} else {
			if (holylib_filesystem_debug.GetBool())
				Msg("GetFileTime: File is not in overridePaths (%s, %s)\n", pFileName, pPathID);
		}
	}

	/*if (holylib_filesystem_predictpath.GetBool())
	{
		CSearchPath* path = NULL;
		std::string strFileName = pFileNameT;
		const char* extension = V_GetFileExtension(pFileNameT);

		bool isModel = false;
		if (V_stricmp(extension, "vvd") == 0)
			isModel = true;

		if (!isModel && V_stricmp(extension, "vtx") == 0) // ToDo: find out which one to keep
			isModel = true;

		if (!isModel && V_stricmp(extension, "phy") == 0)
			isModel = true;

		if (isModel)
		{
			std::string mdlPath = nukeFileExtension(strFileName) + ".mdl";
			path = GetPathFromSearchCache(mdlPath.c_str());
		}

		if (path)
		{
			long time = detour_CBaseFileSystem_GetFileTime.GetTrampoline<Symbols::CBaseFileSystem_GetFileTime>()(filesystem, pFileName, pPathID);
			if (time != 0L) {
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Found file in predicted path! (%s, %s)\n", pFileName, pPathID);
				return time;
			} else
				if (holylib_filesystem_debug.GetBool())
					Msg("OpenForRead: Failed to predict file path! (%s, %s)\n", pFileName, pPathID);
		} else {
			if (holylib_filesystem_debug.GetBool())
				Msg("OpenForRead: Not predicting it! (%s, %s, %s)\n", pFileName, pPathID, extension);
		}
	}*/

	return detour_CBaseFileSystem_GetFileTime.GetTrampoline<Symbols::CBaseFileSystem_GetFileTime>()(filesystem, pFileName, pPathID);
}

static bool gBlockRemoveAllMapPaths = false;
static Detouring::Hook detour_CBaseFileSystem_RemoveAllMapSearchPaths;
static void hook_CBaseFileSystem_RemoveAllMapSearchPaths(IFileSystem* filesystem)
{
	if (gBlockRemoveAllMapPaths)
		return;

	detour_CBaseFileSystem_RemoveAllMapSearchPaths.GetTrampoline<Symbols::CBaseFileSystem_RemoveAllMapSearchPaths>()(filesystem);
}

static std::string getVPKFile(const std::string& fileName) {
	size_t lastThingyPos = fileName.find_last_of('/');
	size_t lastDotPos = fileName.find_last_of('.');

	return fileName.substr(lastThingyPos + 1, lastDotPos - lastThingyPos - 1);
}

static Detouring::Hook detour_CBaseFileSystem_AddSearchPath;
static void hook_CBaseFileSystem_AddSearchPath(IFileSystem* filesystem, const char *pPath, const char *pathID, SearchPathAdd_t addType)
{
	VPROF_BUDGET("HolyLib - CBaseFileSystem::AddSearchPath", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, pathID, addType);

	std::string extension = getFileExtension(pPath);
	/*if (extension == "bsp") {
		const char* pPathID = "__TEMP_MAP_PATH";
		gBlockRemoveAllMapPaths = true;
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, pPathID, addType);

		if (filesystem->IsDirectory("materials/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MATERIALS", addType);

		if (filesystem->IsDirectory("models/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MODELS", addType);
	
		if (filesystem->IsDirectory("sound/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_SOUNDS", addType);
	
		if (filesystem->IsDirectory("maps/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MAPS", addType);
	
		if (filesystem->IsDirectory("resource/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_RESOURCE", addType);

		if (filesystem->IsDirectory("scripts/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_SCRIPTS", addType);

		if (filesystem->IsDirectory("cfg/", pPathID))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_CONFIGS", addType);
		
		gBlockRemoveAllMapPaths = false;
		filesystem->RemoveSearchPath(pPath, pPathID);
	}*/

	std::string strPath = pPath;
	if (V_stricmp(pathID, "GAME") == 0)
	{
		if (filesystem->IsDirectory((strPath + "/materials").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MATERIALS", addType);

		if (filesystem->IsDirectory((strPath + "/models").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MODELS", addType);
	
		if (filesystem->IsDirectory((strPath + "/sound").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_SOUNDS", addType);
	
		if (filesystem->IsDirectory((strPath + "/maps").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_MAPS", addType);
	
		if (filesystem->IsDirectory((strPath + "/resource").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_RESOURCE", addType);

		if (filesystem->IsDirectory((strPath + "/scripts").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_SCRIPTS", addType);

		if (filesystem->IsDirectory((strPath + "/cfg").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "CONTENT_CONFIGS", addType);

		if (filesystem->IsDirectory((strPath + "/gamemodes").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_GAMEMODES", addType);

		if (filesystem->IsDirectory((strPath + "/lua/includes").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_INCLUDES", addType);
	}
	
	if(V_stricmp(pathID, "lsv") == 0 || V_stricmp(pathID, "GAME") == 0)
	{
		if (filesystem->IsDirectory((strPath + "/sandbox").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_GAMEMODE_SANDBOX", addType);

		if (filesystem->IsDirectory((strPath + "/effects").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_EFFECTS", addType);
	
		if (filesystem->IsDirectory((strPath + "/entities").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_ENTITIES", addType);

		if (filesystem->IsDirectory((strPath + "/weapons").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_WEAPONS", addType);

		if (filesystem->IsDirectory((strPath + "/lua/derma").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_DERMA", addType);

		if (filesystem->IsDirectory((strPath + "/lua/drive").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_DRIVE", addType);

		if (filesystem->IsDirectory((strPath + "/lua/entities").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_LUA_ENTITIES", addType);

		if (filesystem->IsDirectory((strPath + "/vgui").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_VGUI", addType);

		if (filesystem->IsDirectory((strPath + "/postprocess").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_POSTPROCESS", addType);

		if (filesystem->IsDirectory((strPath + "/matproxy").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_MATPROXY", addType);

		if (filesystem->IsDirectory((strPath + "/autorun").c_str()))
			detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(filesystem, pPath, "LUA_AUTORUN", addType);
	}

	if (holylib_filesystem_debug.GetBool())
		Msg("Added Searchpath: %s %s %i\n", pPath, pathID, (int)addType);
}

static Detouring::Hook detour_CBaseFileSystem_AddVPKFile;
static void hook_CBaseFileSystem_AddVPKFile(IFileSystem* filesystem, const char *pPath, const char *pathID, SearchPathAdd_t addType)
{
	VPROF_BUDGET("HolyLib - CBaseFileSystem::AddVPKFile", VPROF_BUDGETGROUP_OTHER_FILESYSTEM);

	detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, pathID, addType);

	if (V_stricmp(pathID, "GAME") == 0)
	{
		std::string vpkPath = getVPKFile(pPath);
		detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, vpkPath.c_str(), addType);

		std::string strPath = pPath;
		if (filesystem->IsDirectory("materials/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_MATERIALS", addType);

		if (filesystem->IsDirectory("models/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_MODELS", addType);
	
		if (filesystem->IsDirectory("sound/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_SOUNDS", addType);
	
		if (filesystem->IsDirectory("maps/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_MAPS", addType);
	
		if (filesystem->IsDirectory("resource/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_RESOURCE", addType);

		if (filesystem->IsDirectory("scripts/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_SCRIPTS", addType);

		if (filesystem->IsDirectory("cfg/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "CONTENT_CONFIGS", addType);

		if (filesystem->IsDirectory("gamemodes/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "LUA_GAMEMODES", addType);

		if (filesystem->IsDirectory("lua/includes/"), vpkPath.c_str())
			detour_CBaseFileSystem_AddVPKFile.GetTrampoline<Symbols::CBaseFileSystem_AddVPKFile>()(filesystem, pPath, "LUA_INCLUDES", addType);
	
		filesystem->RemoveSearchPath(pPath, vpkPath.c_str());
	}

	if (holylib_filesystem_debug.GetBool())
		Msg("Added vpk: %s %s %i\n", pPath, pathID, (int)addType);
}

void CFileSystemModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	// We use MOD_WRITE because it doesn't have additional junk search paths.
	g_pOverridePaths["cfg/server.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/banned_ip.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/banned_user.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/skill2.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/game.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/trusted_keys_base.txt"] = "MOD_WRITE";
	g_pOverridePaths["cfg/pure_server_minimal.txt"] = "MOD_WRITE";
	g_pOverridePaths["cfg/skill_manifest.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/skill.cfg"] = "MOD_WRITE";
	g_pOverridePaths["cfg/mapcycle.txt"] = "MOD_WRITE";

	g_pOverridePaths["stale.txt"] = "MOD_WRITE";
	g_pOverridePaths["garrysmod.ver"] = "MOD_WRITE";
	g_pOverridePaths["scripts/actbusy.txt"] = "MOD_WRITE";
	g_pOverridePaths["modelsounds.cache"] = "MOD_WRITE";
	g_pOverridePaths["lua/send.txt"] = "MOD_WRITE";

	g_pOverridePaths["resource/serverevents.res"] = "MOD_WRITE";
	g_pOverridePaths["resource/gameevents.res"] = "MOD_WRITE";
	g_pOverridePaths["resource/modevents.res"] = "MOD_WRITE";
	g_pOverridePaths["resource/hltvevents.res"] = "MOD_WRITE";

	//g_pOverridePaths["scripts/voicecommands.txt"] = "MOD_WRITE";
	//g_pOverridePaths["scripts/propdata.txt"] = "MOD_WRITE";
	//g_pOverridePaths["scripts/propdata/base.txt"] = "MOD_WRITE";
	//g_pOverridePaths["scripts/propdata/cs.txt"] = "MOD_WRITE";
	//g_pOverridePaths["scripts/propdata/l4d.txt"] = "MOD_WRITE";
	//g_pOverridePaths["scripts/propdata/phx.txt"] = "MOD_WRITE";

	if ( pBaseLength < 3 )
		pBaseLength = g_pFullFileSystem->GetSearchPath( "BASE_PATH", true, pBaseDir, sizeof( pBaseDir ) );

	std::string workshopDir = pBaseDir;
		workshopDir = workshopDir + "garrysmod/workshop";

	if (!detour_CBaseFileSystem_AddSearchPath.IsValid())
	{
		Msg("holylib: CBaseFileSystem::AddSearchPath detour is invalid?\n");
		return;
	}

	// NOTE: Check the thing below again
	if (g_pFullFileSystem->IsDirectory("materials/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_MATERIALS", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("models/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_MODELS", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (g_pFullFileSystem->IsDirectory("sound/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_SOUNDS", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (g_pFullFileSystem->IsDirectory("maps/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_MAPS", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (g_pFullFileSystem->IsDirectory("resource/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_RESOURCE", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("scripts/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_SCRIPTS", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("cfg/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "CONTENT_CONFIGS", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("gamemodes/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_GAMEMODES", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("lua/includes/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_INCLUDES", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (g_pFullFileSystem->IsDirectory("sandbox/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_GAMEMODE_SANDBOX", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("effects/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_EFFECTS", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (g_pFullFileSystem->IsDirectory("entities/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_ENTITIES", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("weapons/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_WEAPONS", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("lua/derma/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_DERMA", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("lua/drive/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_DRIVE", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("lua/entities/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_LUA_ENTITIES", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("vgui/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_VGUI", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("postprocess/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_POSTPROCESS", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("matproxy/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_MATPROXY", SearchPathAdd_t::PATH_ADD_TO_TAIL);

	if (g_pFullFileSystem->IsDirectory("autorun/", "workshop"))
		detour_CBaseFileSystem_AddSearchPath.GetTrampoline<Symbols::CBaseFileSystem_AddSearchPath>()(g_pFullFileSystem, workshopDir.c_str(), "LUA_AUTORUN", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	
	if (holylib_filesystem_debug.GetBool())
		Msg("Updated workshop path. (%s)\n", workshopDir.c_str());
}

static CUtlSymbolTableMT* g_pPathIDTable;
inline const char* CPathIDInfo::GetPathIDString() const
{
	return g_pPathIDTable->String( m_PathID );
}


inline const char* CSearchPath::GetPathIDString() const
{
	return m_pPathIDInfo->GetPathIDString();
}

inline const char* CSearchPath::GetPathString() const
{
	return g_pPathIDTable->String( m_Path );
}

void CFileSystemModule::LuaInit(bool bServerInit)
{
	if (!bServerInit)
	{
	}
}

void CFileSystemModule::LuaShutdown()
{
}

void CFileSystemModule::InitDetour(bool bPreServer)
{
	if ( !bPreServer ) { return; }

	SourceSDK::ModuleLoader dedicated_loader("dedicated_srv");
	Detour::Create(
		&detour_CBaseFileSystem_FindFileInSearchPath, "CBaseFileSystem::FindFileInSearchPath",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_FindFileInSearchPathSym,
		(void*)hook_CBaseFileSystem_FindFileInSearchPath, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_IsDirectory, "CBaseFileSystem::IsDirectory",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_IsDirectorySym,
		(void*)hook_CBaseFileSystem_IsDirectory, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_FastFileTime, "CBaseFileSystem::FastFileTime",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_FastFileTimeSym,
		(void*)hook_CBaseFileSystem_FastFileTime, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_FixUpPath, "CBaseFileSystem::FixUpPath",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_FixUpPathSym,
		(void*)hook_CBaseFileSystem_FixUpPath, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_OpenForRead, "CBaseFileSystem::OpenForRead",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_OpenForReadSym,
		(void*)hook_CBaseFileSystem_OpenForRead, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_GetFileTime, "CBaseFileSystem::GetFileTime",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_GetFileTimeSym,
		(void*)hook_CBaseFileSystem_GetFileTime, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_AddSearchPath, "CBaseFileSystem::AddSearchPath",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_AddSearchPathSym,
		(void*)hook_CBaseFileSystem_AddSearchPath, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_AddVPKFile, "CBaseFileSystem::AddVPKFile",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_AddVPKFileSym,
		(void*)hook_CBaseFileSystem_AddVPKFile, m_pID
	);

	Detour::Create(
		&detour_CBaseFileSystem_RemoveAllMapSearchPaths, "CBaseFileSystem::RemoveAllMapSearchPaths",
		dedicated_loader.GetModule(), Symbols::CBaseFileSystem_RemoveAllMapSearchPathsSym,
		(void*)hook_CBaseFileSystem_RemoveAllMapSearchPaths, m_pID
	);

	func_CBaseFileSystem_FindSearchPathByStoreId = (Symbols::CBaseFileSystem_FindSearchPathByStoreId)Detour::GetFunction(dedicated_loader.GetModule(), Symbols::CBaseFileSystem_FindSearchPathByStoreIdSym);
	Detour::CheckFunction(func_CBaseFileSystem_FindSearchPathByStoreId, "CBaseFileSystem::FindSearchPathByStoreId");

	SourceSDK::FactoryLoader dedicated_factory("dedicated_srv");
	g_pPathIDTable = ResolveSymbol<CUtlSymbolTableMT>(dedicated_factory, Symbols::g_PathIDTableSym);
	Detour::CheckValue("get class", "g_PathIDTable", g_pPathIDTable != NULL);
}

void CFileSystemModule::Think(bool simulating)
{
}

void CFileSystemModule::Shutdown()
{
	Detour::Remove(m_pID);
}
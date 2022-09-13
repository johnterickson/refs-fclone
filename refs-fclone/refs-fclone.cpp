// refs-fclone.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "Windows.h"
#include "Shlwapi.h"
#pragma comment(lib, "Shlwapi.lib")

/*main info here
	https://msdn.microsoft.com/library/windows/desktop/mt590821(v=vs.85).aspx
	Change Target platform to 10.0.10586.0 in project > properties > General
	Some copy pasting from this (which does basically the same, the exercise is to understand better, not to copycat) https://github.com/0xbadfca11/reflink/blob/master/reflink.cpp


typedef struct _DUPLICATE_EXTENTS_DATA {
HANDLE        FileHandle;
LARGE_INTEGER SourceFileOffset;
LARGE_INTEGER TargetFileOffset;
LARGE_INTEGER ByteCount;
} DUPLICATE_EXTENTS_DATA, *PDUPLICATE_EXTENTS_DATA;

to clone
BOOL
WINAPI
DeviceIoControl( (HANDLE)       hDevice,          // handle to device
FSCTL_DUPLICATE_EXTENTS_TO_FILE, // dwIoControlCode
(LPVOID)       lpInBuffer,       // input buffer
(DWORD)        nInBufferSize,    // size of input buffer
NULL,                            // lpOutBuffer
0,                               // nOutBufferSize
(LPDWORD)      lpBytesReturned,  // number of bytes returned
(LPOVERLAPPED) lpOverlapped );   // OVERLAPPED structure


*/

#define SUPERMAXPATH 4096
#define ERRORWIDTH 4096
#define CLONESZ 1073741824

//just a generic function to print out the last error in a readable format
void printLastError(LPCWSTR errdetails) {
	wchar_t errorbuffer[ERRORWIDTH];
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorbuffer, ERRORWIDTH, NULL);
	wprintf(L"%ls : %ls\n", errdetails,errorbuffer);
}

wchar_t src[SUPERMAXPATH] = { 0 };
wchar_t src_tmp[SUPERMAXPATH] = { 0 };
wchar_t tgt_prefix[SUPERMAXPATH] = { 0 };
volatile HANDLE hEvent;
volatile LONG threadsReady;
volatile HANDLE hMutex;

#define RETURN_ERROR {DWORD err = GetLastError();  printf("FAILED AT %d: 0x%x\n", __LINE__, err); return err;}

DWORD WINAPI clone(LPVOID lpThreadParameter)
{
	InterlockedIncrement(&threadsReady);
	WaitForSingleObject(hEvent, INFINITE);

	DWORD thread = (ULONG)(ULONG_PTR)lpThreadParameter;

	for (int i = 0; i < 8175 / 25; i++) {

		wchar_t* tgt = new wchar_t[SUPERMAXPATH];

		wsprintf(tgt, L"%s_%d_%d", tgt_prefix, thread, i);


		//return code != 0 => error
		int returncode = 0;

		//some api call require a pointer to a dword although it is not used
		LPDWORD dummyptr = { 0 };

		//wprintf(L"Cloning %s to %s.\n", src, tgt);

		//check if source exits and make sure target does not exists
		if (!PathFileExists(src)) {
			wprintf(L"Src does not exists %s\n", src);
			return 12;
		}
		if (PathFileExists(tgt)) {
			if (!DeleteFile(tgt)) {
				RETURN_ERROR;
			}
		}

		//getting the full path (although it is just for visualisation)
		wchar_t fullsrc[SUPERMAXPATH] = { 0 };
		TCHAR** lppsrcPart = { NULL };
		wchar_t fulltgt[SUPERMAXPATH] = { 0 };
		TCHAR** lpptgtPart = { NULL };

		GetFullPathName(src, SUPERMAXPATH, fullsrc, lppsrcPart);
		GetFullPathName(tgt, SUPERMAXPATH, fulltgt, lpptgtPart);

		//opening the source path for reading
		HANDLE srchandle = CreateFile(fullsrc, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

		//if we can open it query details
		if (srchandle == INVALID_HANDLE_VALUE) {
			RETURN_ERROR;
		}

		//what is the file size
		LARGE_INTEGER filesz = { 0 };
		if (!GetFileSizeEx(srchandle, &filesz)) {
			RETURN_ERROR;
		}

		//basic file info, required to check if the file is sparse (copied from other project)
		FILE_BASIC_INFO filebasicinfo = { 0 };
		if (!GetFileInformationByHandleEx(srchandle, FileBasicInfo, &filebasicinfo, sizeof(filebasicinfo))) {
			RETURN_ERROR;
		}

		//check if the filesystem allows cloning
		ULONG fsflags = 0;
		if (!GetVolumeInformationByHandleW(srchandle, NULL, 0, NULL, NULL, &fsflags, NULL, 0)) {
			RETURN_ERROR;
		}

		if ((fsflags & FILE_SUPPORTS_BLOCK_REFCOUNTING) == 0)
		{
			return 1000;
		}

		//opening the target file for writing
		//create always
		HANDLE tgthandle = CreateFile(fulltgt, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (tgthandle == INVALID_HANDLE_VALUE) {
			printLastError(L"Issue opening target handle");
			RETURN_ERROR;
		}

		//https://technet.microsoft.com/en-us/windows-server-docs/storage/refs/block-cloning
		//must be sparse or not sparse
		//must have same integrity or not
		//must have same file length
		DWORD written = 0;

		//make sure the file is sparse if the sourse is sparse
		//it makes sense that target is sparse so it doesn't consume disk space for blocks that are 0
		if (filebasicinfo.FileAttributes | FILE_ATTRIBUTE_SPARSE_FILE) {
			//printf("Original file is sparse, copying\n");
			FILE_SET_SPARSE_BUFFER sparse = { true };
			if (!DeviceIoControl(tgthandle, FSCTL_SET_SPARSE, &sparse, sizeof(sparse), NULL, 0, dummyptr, NULL)) {
				RETURN_ERROR;
			}
		}

		DWORD clusterSize;
		//integrity scan info must be the same for block cloning in both files
		{
			FSCTL_GET_INTEGRITY_INFORMATION_BUFFER integinfo = { 0 };

			//query the info from src
			if (!DeviceIoControl(srchandle, FSCTL_GET_INTEGRITY_INFORMATION, NULL, 0, &integinfo, sizeof(integinfo), &written, NULL)) {
				RETURN_ERROR;
			}

			//printf("Copied integrity info (%d)\n", integinfo.ChecksumAlgorithm);
			clusterSize = integinfo.ClusterSizeInBytes;

			//setting the info to tgt
			if (!DeviceIoControl(tgthandle, FSCTL_SET_INTEGRITY_INFORMATION, &integinfo, sizeof(integinfo), NULL, 0, dummyptr, NULL)) {
				RETURN_ERROR;
			}
		}

		//setting the file end of the target to the size of the source
		//this basically makes the file as big as the source instead of 0kb, before cloning
		//sparse setting should be done first so it doesn't consume space on disk
		FILE_END_OF_FILE_INFO preallocsz = { filesz };
		//printf("Setting file end at %lld\n", filesz.QuadPart);
		if (!SetFileInformationByHandle(tgthandle, FileEndOfFileInfo, &preallocsz, sizeof(preallocsz))) {
			RETURN_ERROR;
		}

		//file handle are setup, we can start cloning
		//wprintf(L"ReFS CP : %ls (%lld) -> %ls\n", fullsrc, filesz.QuadPart, fulltgt);


		LONGLONG mask = ((LONGLONG)clusterSize) - 1;
		filesz.QuadPart = (filesz.QuadPart + mask) & ~mask;

		//longlong required because basic long is only value of +- 4GB
		//also the block clone require large integers which are basically struct with longlong integers for 64bit server
		//the clone also copies a max of 4GB per time, however here it is limited to CLONESZ
		for (LONGLONG cpoffset = 0; cpoffset < filesz.QuadPart; cpoffset += CLONESZ) {
			LONGLONG cpblocks = CLONESZ;

			//if the offset + the amount of blocks is bigger then CLONESZ, we need to copy a smaller amount
			if ((cpoffset + cpblocks) > filesz.QuadPart) {
				cpblocks = filesz.QuadPart - cpoffset;
			}


			//setting up the struct. since we want identical files, we put the offset the same
			DUPLICATE_EXTENTS_DATA_EX clonestruct = { 0 };
			clonestruct.Size = sizeof(DUPLICATE_EXTENTS_DATA_EX);
			clonestruct.FileHandle = srchandle;
			clonestruct.ByteCount.QuadPart = cpblocks;
			clonestruct.SourceFileOffset.QuadPart = cpoffset;
			clonestruct.TargetFileOffset.QuadPart = cpoffset;
			clonestruct.Flags = 0;

			//wprintf(L"Cloning offset %lld size %lld\n", clonestruct.SourceFileOffset.QuadPart, clonestruct.ByteCount.QuadPart);

			//calling the duplicate "API" with out previous defined struct
			//WaitForSingleObject(hMutex, INFINITE);
			if (!DeviceIoControl(tgthandle, FSCTL_DUPLICATE_EXTENTS_TO_FILE_EX, &clonestruct, sizeof(clonestruct), NULL, 0, dummyptr, NULL)) {
				RETURN_ERROR;
			}
			//ReleaseMutex(hMutex);
		}

		CloseHandle(tgthandle);
		CloseHandle(srchandle);

		tgthandle = CreateFile(fulltgt, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (tgthandle == INVALID_HANDLE_VALUE) {
			printLastError(L"Issue opening target handle");
			RETURN_ERROR;
		}

		BYTE contents[100] = { 0 };
		DWORD bytesRead;
		if (!ReadFile(tgthandle, contents, _countof(contents), &bytesRead, NULL)) {
			RETURN_ERROR;
		}

		if (bytesRead == 0) {
			return 1001;
		}

		BYTE merged = 0;
		for (DWORD i = 0; i < bytesRead; i++) {
			merged |= contents[i];
		}

		if (merged == 0) {
			wprintf(L"All zeros: %s\n", fulltgt);
			fflush(stdout);
			__debugbreak();
			return 1002;
		}

		CloseHandle(tgthandle);

		DeleteFile(fulltgt);
		delete tgt;
	}
	return 0;
}

int main(int argc, char* argv[])
{
	//return code != 0 => error
	int returncode = 0;

	//need at least src and dest file
	if (argc > 2) {
		//converting regular char* to wide char because most windows api call accept it
		MultiByteToWideChar(0, 0, argv[1], (int)strlen(argv[1]), src, (int)strlen(argv[1]));
		wsprintf(src_tmp, L"%s.tmp", src);

		MultiByteToWideChar(0, 0, argv[2], (int)strlen(argv[2]), tgt_prefix, (int)strlen(argv[2]));

		hMutex = CreateMutexW(NULL, FALSE, NULL);
		while (TRUE)
		{
			if (PathFileExistsW(src_tmp)) {
				if (!DeleteFileW(src_tmp)) {
					RETURN_ERROR;
				}
			}

			if (!CopyFileW(src, src_tmp, TRUE)) {
				RETURN_ERROR;
			}

			if (!DeleteFileW(src)) {
				RETURN_ERROR;
			}

			if (!MoveFileW(src_tmp, src)) {
				RETURN_ERROR;
			}
			InterlockedExchange(&threadsReady, 0);
			printf("Repro attempt...");
			hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (hEvent == NULL) {
				RETURN_ERROR;
			}

			HANDLE hThreads[25] = { 0 };
			for (DWORD i = 0; i < _countof(hThreads); i++)
			{
				hThreads[i] = CreateThread(NULL, 0, clone, (LPVOID)(ULONG_PTR)i, 0, NULL);
				if (hThreads[i] == NULL) {
					RETURN_ERROR;
				}
			}

			while (InterlockedOr(&threadsReady,0) != _countof(hThreads))
			{
				SwitchToThread();
			}

			if (!SetEvent(hEvent)) {
				RETURN_ERROR;
			}

			for (DWORD i = 0; i < _countof(hThreads); i++) {
				DWORD exitCode;
				WaitForSingleObject(hThreads[i], INFINITE);
				if (!GetExitCodeThread(hThreads[i], &exitCode)) {
					RETURN_ERROR;
				}

				if (exitCode != 0) {
					printf("ERROR thread %d exit code: 0x%x", i, exitCode);
					return exitCode;

				}
			}
		}
	}
	else {
		printf("refs-fclone.exe <src> <tgt>\n");
		returncode = 11;
	}
    return returncode;
}


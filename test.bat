SETLOCAL ENABLEDELAYEDEXPANSION

for /L %%I IN (1,1,1000) DO (
	del R:\hello.txt & echo hello>R:\hello.txt & del /q  R:\test\*
	D:\src\refs-fclone\x64\Debug\refs-fclone.exe R:\hello.txt R:\test\blob
	if "!ERRORLEVEL!" NEQ "0" exit !ERRORLEVEL!
)
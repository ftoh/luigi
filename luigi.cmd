@echo off

setlocal
cd %~dp0

setlocal
cd dev
call npm install
call node_modules\.bin\cmake-js
endlocal

echo --------------------------------------------------
node src/luigi/luigi.js %*
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$VenvActivate = Join-Path $PSScriptRoot ".venv\Scripts\Activate.ps1"

Set-Location $ProjectRoot
. $VenvActivate

Write-Host "VIRTUAL_ENV = $env:VIRTUAL_ENV"
python --version
pip --version
python -c "import platform, struct; print('arch =', platform.architecture()[0], '| bits =', struct.calcsize('P')*8)"
python -c "import PyQt5, serial; from PyQt5 import QtWidgets, QtCore; print('PyQt5', QtCore.QT_VERSION_STR, '| pyserial', serial.__version__, '| OK')"
python -c "import pyqt5_tools, os; print('designer =', os.path.join(os.path.dirname(pyqt5_tools.__file__), 'Qt', 'bin', 'designer.exe'))"
pyuic5 --version

const { app, BrowserWindow, ipcMain, dialog } = require('electron');
const path = require('path');
const { spawn } = require('child_process');

// Disable sandbox for Linux compatibility
app.commandLine.appendSwitch('no-sandbox');
app.commandLine.appendSwitch('disable-gpu-sandbox');

let mainWindow;
let pythonProcess = null;

const BACKEND_PORT = 5050;
const BACKEND_URL = `http://127.0.0.1:${BACKEND_PORT}`;

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 1400,
        height: 900,
        minWidth: 1000,
        minHeight: 700,
        webPreferences: {
            nodeIntegration: false,
            contextIsolation: true,
            sandbox: false,  // Allow Node.js modules in preload
            preload: path.join(__dirname, 'preload.js')
        },
        backgroundColor: '#0f0f1a',
        titleBarStyle: 'default',
        show: false
    });

    mainWindow.loadFile('index.html');
    
    mainWindow.once('ready-to-show', () => {
        mainWindow.show();
    });

    mainWindow.on('closed', () => {
        mainWindow = null;
    });
}

function startPythonBackend() {
    const backendDir = path.join(__dirname, '..', 'backend');
    const venvPython = path.join(backendDir, '.venv', 'bin', 'python');
    const serverScript = path.join(backendDir, 'server.py');

    pythonProcess = spawn(venvPython, [serverScript], {
        cwd: backendDir,
        stdio: ['ignore', 'pipe', 'pipe']
    });

    pythonProcess.stdout.on('data', (data) => {
        console.log(`Backend: ${data}`);
    });

    pythonProcess.stderr.on('data', (data) => {
        console.error(`Backend Error: ${data}`);
    });

    pythonProcess.on('error', (error) => {
        console.error('Failed to start backend:', error);
    });

    pythonProcess.on('close', (code) => {
        console.log(`Backend exited with code ${code}`);
        pythonProcess = null;
    });
}

function stopPythonBackend() {
    if (pythonProcess) {
        pythonProcess.kill('SIGTERM');
        pythonProcess = null;
    }
}

// IPC Handlers
ipcMain.handle('select-pdf', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
        properties: ['openFile'],
        filters: [{ name: 'PDF Files', extensions: ['pdf'] }]
    });
    
    if (!result.canceled && result.filePaths.length > 0) {
        return result.filePaths[0];
    }
    return null;
});

ipcMain.handle('save-pdf', async (event, defaultName) => {
    const result = await dialog.showSaveDialog(mainWindow, {
        defaultPath: defaultName || 'filtered_output.pdf',
        filters: [{ name: 'PDF Files', extensions: ['pdf'] }]
    });
    
    if (!result.canceled) {
        return result.filePath;
    }
    return null;
});

ipcMain.handle('get-backend-url', () => {
    return BACKEND_URL;
});

// App lifecycle
app.whenReady().then(() => {
    startPythonBackend();
    
    // Wait for backend to start
    setTimeout(createWindow, 1500);
});

app.on('window-all-closed', () => {
    stopPythonBackend();
    if (process.platform !== 'darwin') {
        app.quit();
    }
});

app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
        createWindow();
    }
});

app.on('before-quit', () => {
    stopPythonBackend();
});

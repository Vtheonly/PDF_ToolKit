const { contextBridge, ipcRenderer } = require('electron');
const fs = require('fs');
const path = require('path');

contextBridge.exposeInMainWorld('electronAPI', {
    selectPdf: () => ipcRenderer.invoke('select-pdf'),
    savePdf: (defaultName) => ipcRenderer.invoke('save-pdf', defaultName),
    getBackendUrl: () => ipcRenderer.invoke('get-backend-url'),
    readFile: (filePath) => {
        return new Promise((resolve, reject) => {
            fs.readFile(filePath, (err, data) => {
                if (err) reject(err);
                else resolve({
                    buffer: data,
                    name: path.basename(filePath)
                });
            });
        });
    }
});

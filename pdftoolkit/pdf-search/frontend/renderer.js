/**
 * PDF Search & Extract - Renderer Process
 * Handles UI logic and API communication
 */

// State
const state = {
    backendUrl: null,
    fileId: null,
    fileName: null,
    totalPages: 0,
    searchResults: null,
    matchedPages: [],       // Pages with direct matches
    pagesWithPadding: [],   // All pages including padding
    selectedPages: new Set(),
    thumbnails: {}
};

// DOM Elements
const elements = {
    statusIndicator: document.getElementById('statusIndicator'),
    statusDot: null,
    statusText: null,
    uploadArea: document.getElementById('uploadArea'),
    fileInfo: document.getElementById('fileInfo'),
    fileName: document.getElementById('fileName'),
    filePages: document.getElementById('filePages'),
    clearFile: document.getElementById('clearFile'),
    keywordsInput: document.getElementById('keywordsInput'),
    caseSensitive: document.getElementById('caseSensitive'),
    searchBtn: document.getElementById('searchBtn'),
    resultsSection: document.getElementById('resultsSection'),
    matchedPagesCount: document.getElementById('matchedPagesCount'),
    totalMatchesCount: document.getElementById('totalMatchesCount'),
    selectedPagesCount: document.getElementById('selectedPagesCount'),
    exportSection: document.getElementById('exportSection'),
    selectAllBtn: document.getElementById('selectAllBtn'),
    deselectAllBtn: document.getElementById('deselectAllBtn'),
    exportBtn: document.getElementById('exportBtn'),
    pageGridHeader: document.getElementById('pageGridHeader'),
    emptyState: document.getElementById('emptyState'),
    loadingState: document.getElementById('loadingState'),
    loadingText: document.getElementById('loadingText'),
    pageGrid: document.getElementById('pageGrid')
};

// Initialize
async function init() {
    elements.statusDot = elements.statusIndicator.querySelector('.status-dot');
    elements.statusText = elements.statusIndicator.querySelector('.status-text');
    
    // Get backend URL
    state.backendUrl = await window.electronAPI.getBackendUrl();
    
    // Wait for backend to be ready
    await waitForBackend();
    
    // Setup event listeners
    setupEventListeners();
}

async function waitForBackend() {
    setStatus('Connecting...', 'processing');
    
    let attempts = 0;
    const maxAttempts = 20;
    
    while (attempts < maxAttempts) {
        try {
            const response = await fetch(`${state.backendUrl}/api/health`);
            if (response.ok) {
                setStatus('Ready', 'ready');
                return;
            }
        } catch (e) {
            // Backend not ready yet
        }
        
        await sleep(500);
        attempts++;
    }
    
    setStatus('Backend Error', 'error');
}

function setStatus(text, type = 'ready') {
    elements.statusText.textContent = text;
    elements.statusDot.className = 'status-dot';
    if (type !== 'ready') {
        elements.statusDot.classList.add(type);
    }
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Event Listeners
function setupEventListeners() {
    // Upload area click
    elements.uploadArea.addEventListener('click', handleUploadClick);
    
    // Drag and drop
    elements.uploadArea.addEventListener('dragover', handleDragOver);
    elements.uploadArea.addEventListener('dragleave', handleDragLeave);
    elements.uploadArea.addEventListener('drop', handleDrop);
    
    // Clear file
    elements.clearFile.addEventListener('click', handleClearFile);
    
    // Keywords input
    elements.keywordsInput.addEventListener('input', updateSearchButtonState);
    
    // Search button
    elements.searchBtn.addEventListener('click', handleSearch);
    
    // Selection controls
    elements.selectAllBtn.addEventListener('click', handleSelectAll);
    elements.deselectAllBtn.addEventListener('click', handleDeselectAll);
    
    // Export button
    elements.exportBtn.addEventListener('click', handleExport);
}

// File Upload Handlers
async function handleUploadClick() {
    const filePath = await window.electronAPI.selectPdf();
    if (filePath) {
        await uploadFile(filePath);
    }
}

function handleDragOver(e) {
    e.preventDefault();
    e.stopPropagation();
    elements.uploadArea.classList.add('dragover');
}

function handleDragLeave(e) {
    e.preventDefault();
    e.stopPropagation();
    elements.uploadArea.classList.remove('dragover');
}

async function handleDrop(e) {
    e.preventDefault();
    e.stopPropagation();
    elements.uploadArea.classList.remove('dragover');
    
    const files = e.dataTransfer.files;
    if (files.length > 0 && files[0].type === 'application/pdf') {
        await uploadFile(files[0].path);
    }
}

async function uploadFile(filePath) {
    showLoading('Uploading PDF...');
    setStatus('Uploading...', 'processing');
    
    try {
        // Read file using preload API
        const fileData = await window.electronAPI.readFile(filePath);
        
        // Create blob from buffer
        const blob = new Blob([fileData.buffer], { type: 'application/pdf' });
        
        const formData = new FormData();
        formData.append('file', blob, fileData.name);
        
        const uploadResponse = await fetch(`${state.backendUrl}/api/upload`, {
            method: 'POST',
            body: formData
        });
        
        if (!uploadResponse.ok) {
            throw new Error('Upload failed');
        }
        
        const result = await uploadResponse.json();
        
        state.fileId = result.file_id;
        state.fileName = result.filename;
        state.totalPages = result.total_pages;
        
        // Update UI
        elements.uploadArea.style.display = 'none';
        elements.fileInfo.style.display = 'block';
        elements.fileName.textContent = state.fileName;
        elements.filePages.textContent = `${state.totalPages} pages`;
        
        updateSearchButtonState();
        setStatus('Ready', 'ready');
        hideLoading();
        
    } catch (error) {
        console.error('Upload error:', error);
        setStatus('Upload Failed', 'error');
        hideLoading();
        showEmptyState();
    }
}

function handleClearFile() {
    // Cleanup on backend
    if (state.fileId) {
        fetch(`${state.backendUrl}/api/cleanup/${state.fileId}`, {
            method: 'DELETE'
        }).catch(console.error);
    }
    
    // Reset state
    state.fileId = null;
    state.fileName = null;
    state.totalPages = 0;
    state.searchResults = null;
    state.matchedPages = [];
    state.pagesWithPadding = [];
    state.selectedPages.clear();
    state.thumbnails = {};
    
    // Reset UI
    elements.uploadArea.style.display = 'block';
    elements.fileInfo.style.display = 'none';
    elements.resultsSection.style.display = 'none';
    elements.exportSection.style.display = 'none';
    elements.pageGridHeader.style.display = 'none';
    elements.keywordsInput.value = '';
    
    updateSearchButtonState();
    showEmptyState();
}

// Search Handler
function updateSearchButtonState() {
    const hasFile = state.fileId !== null;
    const hasKeywords = elements.keywordsInput.value.trim().length > 0;
    elements.searchBtn.disabled = !(hasFile && hasKeywords);
}

async function handleSearch() {
    const keywordsText = elements.keywordsInput.value.trim();
    if (!keywordsText || !state.fileId) return;
    
    // Parse keywords (comma or newline separated)
    const keywords = keywordsText
        .split(/[,\n]/)
        .map(k => k.trim())
        .filter(k => k.length > 0);
    
    if (keywords.length === 0) return;
    
    showLoading('Searching keywords...');
    setStatus('Searching...', 'processing');
    
    try {
        const response = await fetch(`${state.backendUrl}/api/search`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                file_id: state.fileId,
                keywords: keywords,
                case_sensitive: elements.caseSensitive.checked
            })
        });
        
        if (!response.ok) {
            throw new Error('Search failed');
        }
        
        const result = await response.json();
        
        state.searchResults = result;
        state.matchedPages = result.matched_pages;
        state.pagesWithPadding = result.pages_with_padding;
        
        // Select all pages by default
        state.selectedPages = new Set(state.pagesWithPadding);
        
        // Update stats
        elements.matchedPagesCount.textContent = result.total_matched_pages;
        elements.totalMatchesCount.textContent = result.match_details.reduce((sum, m) => sum + m.match_count, 0);
        updateSelectedCount();
        
        // Show sections
        elements.resultsSection.style.display = 'block';
        elements.exportSection.style.display = 'block';
        elements.pageGridHeader.style.display = 'flex';
        
        // Load thumbnails
        await loadThumbnails();
        
        // Render page grid
        renderPageGrid();
        
        setStatus('Ready', 'ready');
        hideLoading();
        
    } catch (error) {
        console.error('Search error:', error);
        setStatus('Search Failed', 'error');
        hideLoading();
    }
}

async function loadThumbnails() {
    elements.loadingText.textContent = 'Loading page previews...';
    
    try {
        const response = await fetch(`${state.backendUrl}/api/previews/${state.fileId}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ pages: state.pagesWithPadding })
        });
        
        if (response.ok) {
            const result = await response.json();
            state.thumbnails = result.thumbnails;
        }
    } catch (error) {
        console.error('Thumbnail load error:', error);
    }
}

// Page Grid
function renderPageGrid() {
    elements.pageGrid.innerHTML = '';
    elements.emptyState.style.display = 'none';
    
    if (state.pagesWithPadding.length === 0) {
        showEmptyState('No matches found');
        return;
    }
    
    // Get match details map
    const matchDetails = {};
    if (state.searchResults && state.searchResults.match_details) {
        state.searchResults.match_details.forEach(m => {
            matchDetails[m.page] = m.match_count;
        });
    }
    
    state.pagesWithPadding.forEach(pageNum => {
        const isMatched = state.matchedPages.includes(pageNum);
        const matchCount = matchDetails[pageNum] || 0;
        const isSelected = state.selectedPages.has(pageNum);
        
        const card = createPageCard(pageNum, isMatched, matchCount, isSelected);
        elements.pageGrid.appendChild(card);
    });
    
    updateExportButtonState();
}

function createPageCard(pageNum, isMatched, matchCount, isSelected) {
    const card = document.createElement('div');
    card.className = 'page-card';
    card.dataset.page = pageNum;
    
    if (isSelected) card.classList.add('selected');
    if (isMatched) {
        card.classList.add('matched');
    } else {
        card.classList.add('padding-page');
    }
    
    // Checkbox
    const checkbox = document.createElement('div');
    checkbox.className = 'page-checkbox';
    checkbox.innerHTML = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3">
        <polyline points="20 6 9 17 4 12"></polyline>
    </svg>`;
    
    // Thumbnail
    const thumbnail = document.createElement('div');
    thumbnail.className = 'page-thumbnail';
    
    if (state.thumbnails[pageNum]) {
        const img = document.createElement('img');
        img.src = state.thumbnails[pageNum];
        img.alt = `Page ${pageNum}`;
        thumbnail.appendChild(img);
    } else {
        const loading = document.createElement('div');
        loading.className = 'loading-thumb';
        thumbnail.appendChild(loading);
        
        // Load thumbnail lazily
        loadSingleThumbnail(pageNum, thumbnail);
    }
    
    // Info
    const info = document.createElement('div');
    info.className = 'page-info';
    
    const pageNumber = document.createElement('span');
    pageNumber.className = 'page-number';
    pageNumber.textContent = `Page ${pageNum}`;
    
    info.appendChild(pageNumber);
    
    if (matchCount > 0) {
        const badge = document.createElement('span');
        badge.className = 'match-badge';
        badge.textContent = `${matchCount} match${matchCount > 1 ? 'es' : ''}`;
        info.appendChild(badge);
    } else {
        const badge = document.createElement('span');
        badge.className = 'match-badge padding-badge';
        badge.textContent = 'context';
        info.appendChild(badge);
    }
    
    card.appendChild(checkbox);
    card.appendChild(thumbnail);
    card.appendChild(info);
    
    // Click handler
    card.addEventListener('click', () => togglePageSelection(pageNum));
    
    return card;
}

async function loadSingleThumbnail(pageNum, container) {
    try {
        const response = await fetch(`${state.backendUrl}/api/preview/${state.fileId}/${pageNum}`);
        if (response.ok) {
            const result = await response.json();
            state.thumbnails[pageNum] = result.thumbnail;
            
            container.innerHTML = '';
            const img = document.createElement('img');
            img.src = result.thumbnail;
            img.alt = `Page ${pageNum}`;
            container.appendChild(img);
        }
    } catch (error) {
        console.error(`Thumbnail load error for page ${pageNum}:`, error);
    }
}

function togglePageSelection(pageNum) {
    if (state.selectedPages.has(pageNum)) {
        state.selectedPages.delete(pageNum);
    } else {
        state.selectedPages.add(pageNum);
    }
    
    const card = document.querySelector(`.page-card[data-page="${pageNum}"]`);
    if (card) {
        card.classList.toggle('selected');
    }
    
    updateSelectedCount();
    updateExportButtonState();
}

function handleSelectAll() {
    state.pagesWithPadding.forEach(pageNum => {
        state.selectedPages.add(pageNum);
        const card = document.querySelector(`.page-card[data-page="${pageNum}"]`);
        if (card) card.classList.add('selected');
    });
    
    updateSelectedCount();
    updateExportButtonState();
}

function handleDeselectAll() {
    state.selectedPages.clear();
    
    document.querySelectorAll('.page-card').forEach(card => {
        card.classList.remove('selected');
    });
    
    updateSelectedCount();
    updateExportButtonState();
}

function updateSelectedCount() {
    elements.selectedPagesCount.textContent = state.selectedPages.size;
}

function updateExportButtonState() {
    elements.exportBtn.disabled = state.selectedPages.size === 0;
}

// Export Handler
async function handleExport() {
    if (state.selectedPages.size === 0) return;
    
    // Get save path from user
    const defaultName = `filtered_${state.fileName || 'output.pdf'}`;
    const savePath = await window.electronAPI.savePdf(defaultName);
    
    if (!savePath) return;
    
    showLoading('Generating PDF...');
    setStatus('Exporting...', 'processing');
    
    try {
        // Sort selected pages
        const sortedPages = Array.from(state.selectedPages).sort((a, b) => a - b);
        
        const response = await fetch(`${state.backendUrl}/api/export`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                file_id: state.fileId,
                pages: sortedPages,
                filename: savePath.split('/').pop()
            })
        });
        
        if (!response.ok) {
            throw new Error('Export failed');
        }
        
        // Get the blob and write to file
        const blob = await response.blob();
        
        // Use File System Access API or fallback
        const buffer = await blob.arrayBuffer();
        
        // Create a download link as fallback
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = savePath.split('/').pop();
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        setStatus('Export Complete!', 'ready');
        hideLoading();
        
        // Reset status after 3 seconds
        setTimeout(() => setStatus('Ready', 'ready'), 3000);
        
    } catch (error) {
        console.error('Export error:', error);
        setStatus('Export Failed', 'error');
        hideLoading();
    }
}

// UI Helpers
function showLoading(text = 'Processing...') {
    elements.loadingText.textContent = text;
    elements.loadingState.style.display = 'flex';
    elements.emptyState.style.display = 'none';
    elements.pageGrid.style.display = 'none';
}

function hideLoading() {
    elements.loadingState.style.display = 'none';
    elements.pageGrid.style.display = 'grid';
}

function showEmptyState(text = null) {
    elements.emptyState.style.display = 'flex';
    elements.pageGrid.style.display = 'none';
    elements.loadingState.style.display = 'none';
    
    if (text) {
        elements.emptyState.querySelector('h2').textContent = text;
    }
}

// Initialize app
document.addEventListener('DOMContentLoaded', init);

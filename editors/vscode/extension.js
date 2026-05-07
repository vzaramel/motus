const vscode = require('vscode');
const { spawn } = require('child_process');
const path = require('path');

let serverProcess = null;
let outputChannel = null;
let pendingRequests = new Map();
let requestId = 0;
let messageBuffer = '';

/**
 * Send a JSON-RPC request to the LSP server
 */
function sendRequest(method, params) {
    return new Promise((resolve, reject) => {
        const id = ++requestId;
        const request = {
            jsonrpc: '2.0',
            id: id,
            method: method,
            params: params
        };
        
        pendingRequests.set(id, { resolve, reject });
        
        const content = JSON.stringify(request);
        const message = `Content-Length: ${Buffer.byteLength(content)}\r\n\r\n${content}`;
        
        serverProcess.stdin.write(message);
    });
}

/**
 * Send a notification (no response expected)
 */
function sendNotification(method, params) {
    const notification = {
        jsonrpc: '2.0',
        method: method,
        params: params
    };
    
    const content = JSON.stringify(notification);
    const message = `Content-Length: ${Buffer.byteLength(content)}\r\n\r\n${content}`;
    
    serverProcess.stdin.write(message);
}

/**
 * Handle incoming LSP messages
 */
function handleMessage(data) {
    messageBuffer += data;
    
    while (messageBuffer.length > 0) {
        // Parse headers
        const headerEnd = messageBuffer.indexOf('\r\n\r\n');
        if (headerEnd === -1) break;
        
        const headerStr = messageBuffer.substring(0, headerEnd);
        const bodyStart = headerEnd + 4;
        
        let contentLength = 0;
        const headerLines = headerStr.split('\r\n');
        for (const line of headerLines) {
            if (line.startsWith('Content-Length:')) {
                contentLength = parseInt(line.substring(15).trim(), 10);
                break;
            }
        }
        
        if (messageBuffer.length < bodyStart + contentLength) {
            break; // Need more data
        }
        
        const body = messageBuffer.substring(bodyStart, bodyStart + contentLength);
        messageBuffer = messageBuffer.substring(bodyStart + contentLength);
        
        try {
            const response = JSON.parse(body);
            
            if (response.id !== undefined) {
                // It's a response to a request
                const pending = pendingRequests.get(response.id);
                if (pending) {
                    pendingRequests.delete(response.id);
                    if (response.error) {
                        pending.reject(new Error(response.error.message));
                    } else {
                        pending.resolve(response.result);
                    }
                }
            } else if (response.method) {
                // It's a notification from server
                handleServerNotification(response.method, response.params);
            }
        } catch (e) {
            console.error('Failed to parse LSP response:', e);
        }
    }
}

/**
 * Handle notifications from the server
 */
function handleServerNotification(method, params) {
    if (method === 'textDocument/publishDiagnostics') {
        const uri = params.uri;
        const diagnostics = params.diagnostics || [];
        
        const vscodeDiags = diagnostics.map(d => {
            return new vscode.Diagnostic(
                new vscode.Range(
                    d.range.start.line,
                    d.range.start.character,
                    d.range.end.line,
                    d.range.end.character
                ),
                d.message,
                d.severity === 1 ? vscode.DiagnosticSeverity.Error :
                d.severity === 2 ? vscode.DiagnosticSeverity.Warning :
                vscode.DiagnosticSeverity.Information
            );
        });
        
        const collection = vscode.languages.createDiagnosticCollection('motus');
        collection.set(vscode.Uri.parse(uri), vscodeDiags);
    }
}

/**
 * @param {vscode.ExtensionContext} context
 */
function activate(context) {
    outputChannel = vscode.window.createOutputChannel('Motus');
    outputChannel.appendLine('Motus extension activating...');
    outputChannel.appendLine('Extension path: ' + context.extensionPath);
    outputChannel.appendLine('CWD: ' + process.cwd());
    
    // Find the workspace root
    let workspaceRoot = null;
    if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
        workspaceRoot = vscode.workspace.workspaceFolders[0].uri.fsPath;
        outputChannel.appendLine('Workspace root: ' + workspaceRoot);
    }
    
    // Find LSP server path
    let lspPath = vscode.workspace.getConfiguration('motus').get('serverPath');
    outputChannel.appendLine('Configured path: ' + (lspPath || 'not set'));
    
    if (!lspPath) {
        // Try default locations - prioritize workspace root
        const possiblePaths = [];
        
        // From workspace root
        if (workspaceRoot) {
            possiblePaths.push(
                path.join(workspaceRoot, 'build', 'mot-lsp'),
                path.join(workspaceRoot, 'bin', 'mot-lsp')
            );
        }
        
        // From extension path - go up to find project
        possiblePaths.push(
            path.join(context.extensionPath, '..', '..', '..', '..', 'build', 'mot-lsp'),
            path.join(context.extensionPath, '..', '..', '..', 'build', 'mot-lsp'),
            path.join(context.extensionPath, '..', '..', 'build', 'mot-lsp'),
            path.join(require('os').homedir(), 'motus', 'build', 'mot-lsp'),
            '/usr/local/bin/mot-lsp'
        );
        
        for (const p of possiblePaths) {
            outputChannel.appendLine('Trying: ' + p);
            try {
                require('fs').accessSync(p);
                lspPath = p;
                outputChannel.appendLine('Found: ' + p);
                break;
            } catch (e) {
                // Not found, try next
            }
        }
    }
    
    if (!lspPath) {
        vscode.window.showWarningMessage(
            'Motus LSP server not found. Please build with "make lsp" and set motus.serverPath in settings.'
        );
        return;
    }

    outputChannel.appendLine('Starting Motus LSP server:', lspPath);

    try {
        serverProcess = spawn(lspPath, [], {
            stdio: ['pipe', 'pipe', 'pipe']
        });
    } catch (e) {
        vscode.window.showErrorMessage('Failed to start Motus LSP server: ' + e.message);
        return;
    }

    serverProcess.stderr.on('data', (data) => {
        outputChannel.appendLine('[LSP stderr]: ' + data.toString());
    });

    serverProcess.on('error', (err) => {
        outputChannel.appendLine('[LSP error]: ' + err.message);
    });

    serverProcess.on('exit', (code) => {
        outputChannel.appendLine('[LSP exited]: code ' + code);
    });

    serverProcess.stdout.on('data', handleMessage);

    // Initialize the server
    sendRequest('initialize', {
        processId: process.pid,
        capabilities: {}
    }).then(() => {
        outputChannel.appendLine('LSP server initialized');
        
        // Send initialized notification
        sendNotification('initialized', {});
    }).catch(err => {
        outputChannel.appendLine('[Init error]: ' + err.message);
    });

    // Register providers
    const disposables = [];
    
    // Completion provider
    disposables.push(vscode.languages.registerCompletionItemProvider(
        { language: 'motus', pattern: '**/*.mot' },
        {
            provideCompletionItems(document, position) {
                return sendRequest('textDocument/completion', {
                    textDocument: { uri: document.uri.toString() },
                    position: { line: position.line, character: position.character }
                });
            }
        },
        '<', ' ', ':'
    ));

    // Hover provider
    disposables.push(vscode.languages.registerHoverProvider(
        { language: 'motus', pattern: '**/*.mot' },
        {
            provideHover(document, position) {
                return sendRequest('textDocument/hover', {
                    textDocument: { uri: document.uri.toString() },
                    position: { line: position.line, character: position.character }
                });
            }
        }
    ));

    // Definition provider
    disposables.push(vscode.languages.registerDefinitionProvider(
        { language: 'motus', pattern: '**/*.mot' },
        {
            provideDefinition(document, position) {
                return sendRequest('textDocument/definition', {
                    textDocument: { uri: document.uri.toString() },
                    position: { line: position.line, character: position.character }
                });
            }
        }
    ));

    // Document symbols provider
    disposables.push(vscode.languages.registerDocumentSymbolProvider(
        { language: 'motus', pattern: '**/*.mot' },
        {
            provideDocumentSymbols(document) {
                return sendRequest('textDocument/documentSymbol', {
                    textDocument: { uri: document.uri.toString() }
                });
            }
        }
    ));

    // On type formatting - auto-close tags
    disposables.push(vscode.languages.registerOnTypeFormattingEditProvider(
        { language: 'motus', pattern: '**/*.mot' },
        {
            provideOnTypeFormattingEdits(document, position, ch, options) {
                const edits = [];
                const line = document.lineAt(position.line);
                const text = line.text;
                
                // When typing >, check if we need to auto-close
                if (ch === '>') {
                    const beforeCursor = text.substring(0, position.character);
                    const match = beforeCursor.match(/<([\w-]+)(?![^<]*>)[^<]*$/);
                    if (match) {
                        const tagName = match[1];
                        // Don't auto-close self-closing tags
                        if (!beforeCursor.endsWith('/') && !tagName.startsWith('/')) {
                            const voidTags = ['br', 'hr', 'img', 'input', 'link', 'meta', 'area', 'base', 'col', 'embed', 'param', 'source', 'track', 'wbr'];
                            if (!voidTags.includes(tagName.toLowerCase())) {
                                edits.push({
                                    range: new vscode.Range(
                                        position.line,
                                        position.character,
                                        position.line,
                                        position.character
                                    ),
                                    newText: '</' + tagName + '>'
                                });
                            }
                        }
                    }
                }
                
                return edits;
            }
        },
        '>'  // Trigger on typing >
    ));

    // Document change handler
    const documents = new Map();
    
    // Function to send didOpen for a document
    function openDocument(doc, uri) {
        outputChannel.appendLine('Opening document: ' + uri);
        sendNotification('textDocument/didOpen', {
            textDocument: {
                uri: uri,
                languageId: 'motus',
                version: doc.version,
                text: doc.getText()
            }
        });
        documents.set(uri, doc);
    }
    
    // Send didOpen for already open documents
    vscode.workspace.textDocuments.forEach(doc => {
        if (doc.languageId === 'motus' || doc.fileName.endsWith('.mot')) {
            openDocument(doc, doc.uri.toString());
        }
    });
    
    disposables.push(vscode.workspace.onDidOpenTextDocument(e => {
        if (e.languageId === 'motus' || e.fileName.endsWith('.mot')) {
            openDocument(e, e.uri.toString());
        }
    }));

    disposables.push(vscode.workspace.onDidChangeTextDocument(e => {
        const uri = e.document.uri.toString();
        if (documents.has(uri)) {
            sendNotification('textDocument/didChange', {
                textDocument: {
                    uri: uri,
                    version: e.document.version
                },
                contentChanges: e.contentChanges.map(change => ({
                    text: change.text
                }))
            });
        }
    }));

    disposables.push(vscode.workspace.onDidCloseTextDocument(e => {
        const uri = e.uri.toString();
        if (documents.has(uri)) {
            sendNotification('textDocument/didClose', {
                textDocument: { uri: uri }
            });
            documents.delete(uri);
        }
    }));

    // Register commands
    function restartLSP() {
        outputChannel.appendLine('Restarting LSP...');
        
        // Kill existing process
        if (serverProcess) {
            sendNotification('exit', {});
            serverProcess.kill();
            serverProcess = null;
        }
        
        // Clear pending requests
        pendingRequests.forEach((req, id) => {
            req.reject(new Error('LSP restarted'));
        });
        pendingRequests.clear();
        requestId = 0;
        
        // Restart
        serverProcess = spawn(lspPath, [], {
            stdio: ['pipe', 'pipe', 'pipe']
        });
        
        serverProcess.stderr.on('data', (data) => {
            outputChannel.appendLine('[LSP stderr]: ' + data.toString());
        });
        
        serverProcess.on('error', (err) => {
            outputChannel.appendLine('[LSP error]: ' + err.message);
        });
        
        serverProcess.on('exit', (code) => {
            outputChannel.appendLine('[LSP exited]: code ' + code);
        });
        
        serverProcess.stdout.on('data', handleMessage);
        
        // Re-initialize
        sendRequest('initialize', {
            processId: process.pid,
            capabilities: {}
        }).then(() => {
            outputChannel.appendLine('LSP restarted successfully');
            
            // Re-send didOpen for all open documents
            documents.forEach((doc, uri) => {
                sendNotification('textDocument/didOpen', {
                    textDocument: {
                        uri: uri,
                        languageId: 'motus',
                        version: doc.version,
                        text: doc.getText()
                    }
                });
            });
        }).catch(err => {
            outputChannel.appendLine('[Restart error]: ' + err.message);
        });
    }
    
    function showOutput() {
        outputChannel.show(true);
    }
    
    function compileCurrentFile() {
        const editor = vscode.window.activeTextEditor;
        if (!editor || !editor.document.fileName.endsWith('.mot')) {
            vscode.window.showWarningMessage('No .mot file open');
            return;
        }
        
        const doc = editor.document;
        const content = doc.getText();
        
        outputChannel.appendLine('Compiling: ' + doc.fileName);
        
        const { exec } = require('child_process');
        const tmpFile = '/tmp/mot_compile_' + Date.now() + '.mot';
        require('fs').writeFileSync(tmpFile, content);
        
        const workspaceRoot = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0] ? vscode.workspace.workspaceFolders[0].uri.fsPath : process.cwd();
        exec(`./build/mot --input ${tmpFile}`, { cwd: workspaceRoot }, (err, stdout, stderr) => {
            if (err) {
                outputChannel.appendLine('[Compile Error]: ' + err.message);
                vscode.window.showErrorMessage('Compilation failed: ' + err.message);
            } else if (stderr) {
                outputChannel.appendLine('[Compile stderr]: ' + stderr);
            } else {
                outputChannel.appendLine('[Compiled successfully]');
                vscode.window.showInformationMessage('Compiled successfully!');
            }
            try { require('fs').unlinkSync(tmpFile); } catch(e) {}
        });
    }
    
    disposables.push(vscode.commands.registerCommand('motus.restartLSP', restartLSP));
    disposables.push(vscode.commands.registerCommand('motus.showOutput', showOutput));
    disposables.push(vscode.commands.registerCommand('motus.compile', compileCurrentFile));

    context.subscriptions.push(...disposables);

    context.subscriptions.push({
        dispose: () => {
            if (serverProcess) {
                sendNotification('exit', {});
                serverProcess.kill();
            }
            if (outputChannel) {
                outputChannel.dispose();
            }
        }
    });
}

function deactivate() {
    if (serverProcess) {
        sendNotification('exit', {});
        serverProcess.kill();
    }
}

module.exports = { activate, deactivate };

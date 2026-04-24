import * as childProcess from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let lspCompletionAvailable = false;

const vscodeLanguageSelector: vscode.DocumentSelector = [
  { language: "nebula", scheme: "file" },
];

const clientDocumentSelector: LanguageClientOptions["documentSelector"] = [
  { language: "nebula", scheme: "file" },
];

const keywordCompletions = [
  "async",
  "await",
  "break",
  "continue",
  "else",
  "enum",
  "extern",
  "fn",
  "for",
  "if",
  "import",
  "let",
  "match",
  "module",
  "region",
  "return",
  "shared",
  "struct",
  "unique",
  "unsafe",
  "ui",
  "view",
  "while",
];

function nebulaBinaryPath(): string {
  return vscode.workspace.getConfiguration("nebula").get<string>("binaryPath", "nebula");
}

function ensureOutputChannel(): vscode.OutputChannel {
  if (outputChannel === undefined) {
    outputChannel = vscode.window.createOutputChannel("Nebula");
  }
  return outputChannel;
}

function runNebula(
  args: string[],
  options: { cwd?: string; input?: string } = {},
): childProcess.SpawnSyncReturns<string> {
  const channel = ensureOutputChannel();
  channel.appendLine(`[cmd] ${nebulaBinaryPath()} ${args.join(" ")}`);
  const result = childProcess.spawnSync(nebulaBinaryPath(), args, {
    cwd: options.cwd,
    input: options.input,
    encoding: "utf8",
  });
  if (result.stdout) {
    channel.appendLine(result.stdout.trimEnd());
  }
  if (result.stderr) {
    channel.appendLine(result.stderr.trimEnd());
  }
  return result;
}

async function startLanguageClient(context: vscode.ExtensionContext): Promise<void> {
  if (client !== undefined) {
    return;
  }

  const serverOptions: ServerOptions = {
    command: nebulaBinaryPath(),
    args: ["lsp"],
    transport: TransportKind.stdio,
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: clientDocumentSelector,
    outputChannel: ensureOutputChannel(),
  };

  client = new LanguageClient("nebula", "Nebula Language Server", serverOptions, clientOptions);
  context.subscriptions.push(client);
  try {
    await client.start();
    lspCompletionAvailable =
      client.initializeResult?.capabilities?.completionProvider !== undefined;
  } catch (error) {
    ensureOutputChannel().appendLine(String(error));
    client = undefined;
    lspCompletionAvailable = false;
    void vscode.window.showWarningMessage(
      "Nebula language server failed to start. Falling back to local editor features.",
    );
  }
}

async function restartLanguageClient(context: vscode.ExtensionContext): Promise<void> {
  if (client !== undefined) {
    const current = client;
    client = undefined;
    lspCompletionAvailable = false;
    await current.stop();
  }
  await startLanguageClient(context);
}

function wholeDocumentRange(document: vscode.TextDocument): vscode.Range {
  const start = new vscode.Position(0, 0);
  const end = document.positionAt(document.getText().length);
  return new vscode.Range(start, end);
}

async function formatNebulaDocument(
  document: vscode.TextDocument,
): Promise<vscode.TextEdit[]> {
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), "nebula-vscode-"));
  const tempPath = path.join(tempRoot, path.basename(document.fileName || "document.nb"));

  try {
    fs.writeFileSync(tempPath, document.getText(), "utf8");
    const result = runNebula(["fmt", tempPath], {
      cwd: vscode.workspace.getWorkspaceFolder(document.uri)?.uri.fsPath,
    });
    if (result.status !== 0) {
      throw new Error(result.stderr || "nebula fmt failed");
    }

    const formatted = fs.readFileSync(tempPath, "utf8");
    if (formatted === document.getText()) {
      return [];
    }

    return [vscode.TextEdit.replace(wholeDocumentRange(document), formatted)];
  } finally {
    fs.rmSync(tempRoot, { recursive: true, force: true });
  }
}

function activeNebulaEditor(): vscode.TextEditor | undefined {
  const editor = vscode.window.activeTextEditor;
  if (editor?.document.languageId !== "nebula") {
    return undefined;
  }
  return editor;
}

async function explainSymbol(): Promise<void> {
  const editor = activeNebulaEditor();
  if (editor === undefined) {
    vscode.window.showWarningMessage("Open a Nebula file first.");
    return;
  }

  const document = editor.document;
  const range = document.getWordRangeAtPosition(editor.selection.active);
  const symbol = range ? document.getText(range) : "";
  const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
  const explainRoot = workspaceFolder?.uri.fsPath ?? document.fileName;
  const line = editor.selection.active.line + 1;
  const col = editor.selection.active.character + 1;
  const args = [
    "explain",
    explainRoot,
    "--file",
    document.fileName,
    "--line",
    String(line),
    "--col",
    String(col),
    "--format",
    "json",
  ];
  if (symbol.length > 0) {
    args.splice(4, 0, "--symbol", symbol);
  }
  const result = runNebula(
    args,
    { cwd: workspaceFolder?.uri.fsPath },
  );

  if (result.status !== 0 || !result.stdout) {
    vscode.window.showErrorMessage(
      symbol.length > 0 ? `Nebula explain failed for ${symbol}.` : "Nebula explain failed at the current cursor position.",
    );
    ensureOutputChannel().show(true);
    return;
  }

  try {
    const parsed = JSON.parse(result.stdout) as {
      symbol_matches?: Array<{ detail: string; path: string; line: number; col: number }>;
      variables?: Array<{ function: string; name: string; rep: string; owner: string }>;
      async?: Array<{
        kind: string;
        function: string;
        summary: string;
        allocation: string;
        carried_values?: string[];
      }>;
      diagnostics?: Array<{ code: string; message: string }>;
    };

    const lines: string[] = [];
    if (symbol.length > 0) {
      lines.push(`symbol: ${symbol}`);
    } else {
      lines.push(`cursor: ${document.fileName}:${line}:${col}`);
    }
    for (const match of parsed.symbol_matches ?? []) {
      lines.push(`match: ${match.detail} @ ${match.path}:${match.line}:${match.col}`);
    }
    for (const variable of parsed.variables ?? []) {
      lines.push(
        `var: fn=${variable.function} name=${variable.name} rep=${variable.rep} owner=${variable.owner}`,
      );
    }
    for (const entry of parsed.async ?? []) {
      const carried =
        entry.carried_values && entry.carried_values.length > 0
          ? ` carried=${entry.carried_values.join(",")}`
          : "";
      lines.push(
        `async: kind=${entry.kind} fn=${entry.function} allocation=${entry.allocation} summary=${entry.summary}${carried}`,
      );
    }
    for (const diagnostic of parsed.diagnostics ?? []) {
      lines.push(`diag: ${diagnostic.code} ${diagnostic.message}`);
    }

    const channel = ensureOutputChannel();
    channel.appendLine(lines.join("\n"));
    channel.show(true);
    void vscode.window.showInformationMessage(
      symbol.length > 0 ? `Nebula explain completed for ${symbol}.` : "Nebula explain completed for the current cursor position.",
    );
  } catch (error) {
    ensureOutputChannel().appendLine(String(error));
    ensureOutputChannel().show(true);
    vscode.window.showErrorMessage("Nebula explain returned invalid JSON.");
  }
}

async function createNewProject(): Promise<void> {
  const parent = await vscode.window.showOpenDialog({
    canSelectFiles: false,
    canSelectFolders: true,
    canSelectMany: false,
    openLabel: "Select Parent Folder",
  });
  if (!parent || parent.length === 0) {
    return;
  }

  const projectName = await vscode.window.showInputBox({
    prompt: "Nebula project name",
    validateInput: (value) => (value.trim().length === 0 ? "Project name is required" : undefined),
  });
  if (!projectName) {
    return;
  }

  const target = path.join(parent[0].fsPath, projectName);
  const result = runNebula(["new", target], { cwd: parent[0].fsPath });
  if (result.status !== 0) {
    vscode.window.showErrorMessage(`Nebula new failed for ${projectName}.`);
    ensureOutputChannel().show(true);
    return;
  }

  void vscode.commands.executeCommand("vscode.openFolder", vscode.Uri.file(target), true);
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  await startLanguageClient(context);

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (event.affectsConfiguration("nebula.binaryPath")) {
        await restartLanguageClient(context);
      }
    }),
  );

  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider(vscodeLanguageSelector, {
      provideDocumentFormattingEdits(document) {
        return formatNebulaDocument(document);
      },
    }),
  );

  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(vscodeLanguageSelector, {
      provideCompletionItems() {
        if (client !== undefined && lspCompletionAvailable) {
          return [];
        }
        return keywordCompletions.map((keyword) => {
          const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
          item.insertText = keyword;
          return item;
        });
      },
    }),
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("nebula.explainSymbol", explainSymbol),
    vscode.commands.registerCommand("nebula.newProject", createNewProject),
    vscode.commands.registerCommand("nebula.restartLanguageServer", async () => {
      await restartLanguageClient(context);
      vscode.window.showInformationMessage("Nebula language server restarted.");
    }),
  );
}

export async function deactivate(): Promise<void> {
  if (client !== undefined) {
    const current = client;
    client = undefined;
    lspCompletionAvailable = false;
    await current.stop();
  }
}

using KillEngine.ClrInspector;

// =============================================================================
// KillEngineClrInspector -- helper .NET pour le candidat 8
// (docs/POWER_UP_ROADMAP.md, ClrMD/SOS), MVP demande explicitement par
// l'utilisateur le 20/08/2026 : detecter/attacher un CLR sur un PID,
// enumerer le heap, retrouver des objets/types connus, lire des champs
// primitifs et references, retrouver au moins une GC root, et demontrer
// qu'apres un forceGC declenche ailleurs, on retrouve le meme objet logique
// malgre un eventuel changement d'adresse. Voir
// docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour l'architecture complete.
// =============================================================================

Console.WriteLine("KillEngineClrInspector demarre.");
Console.WriteLine($"PID: {Environment.ProcessId}");

// Nom de pipe surchargeable par variable d'environnement -- necessaire pour
// que les auto-tests de regression (kill abrupt, redemarrage nouveau PID)
// puissent lancer des instances supplementaires isolees sans collision avec
// une instance deja active sur le nom par defaut.
string pipeName = Environment.GetEnvironmentVariable("KILLENGINE_CLR_INSPECTOR_PIPE_NAME")
    ?? ControlPipeServer.DefaultPipeName;

string markerFileName = pipeName == ControlPipeServer.DefaultPipeName
    ? "killengine_clr_inspector_addresses.txt"
    : $"killengine_clr_inspector_addresses_{pipeName}.txt";
var markerPath = Path.Combine(Path.GetTempPath(), markerFileName);
File.WriteAllText(
    markerPath,
    $"pid={Environment.ProcessId}\npipeName={pipeName}\n",
    new System.Text.UTF8Encoding(false));
Console.WriteLine($"Marqueur ecrit : {markerPath}");

using var cts = new CancellationTokenSource();

Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    Console.WriteLine("Ctrl+C recu, arret en cours...");
    cts.Cancel();
};

using var session = new ClrSession();
var dispatcher = new MethodDispatcher(session, requestShutdown: () => cts.Cancel());
var pipeServer = new ControlPipeServer(dispatcher, pipeName);

try
{
    await pipeServer.RunAsync(cts.Token).ConfigureAwait(false);
}
finally
{
    try { File.Delete(markerPath); } catch { /* best-effort */ }
    Console.WriteLine("KillEngineClrInspector arrete.");
}

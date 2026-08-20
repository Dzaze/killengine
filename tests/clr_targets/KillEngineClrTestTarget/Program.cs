using KillEngine.ClrTestTarget;

// =============================================================================
// KillEngineClrTestTarget — cible de test CLR dediee et reproductible
//
// Contrairement a KillEngineTestTarget.exe (C++/Qt, tests/memory_targets/),
// ce process est un vrai CoreCLR standard : sert a developper et valider le
// futur candidat #8 (ClrMD/SOS, docs/POWER_UP_ROADMAP.md) independamment de
// Solitaire ou de tout autre logiciel tiers -- voir
// docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md pour le cahier des charges complet
// et pourquoi ce chantier existe (l'investigation Solitaire du 20/08/2026 a
// prouve que Solitaire.exe n'est PAS une cible CLR, docs/STRATEGY_ROOM.md).
// =============================================================================

Console.WriteLine("KillEngineClrTestTarget demarre.");
Console.WriteLine($"PID: {Environment.ProcessId}");

// Force la construction du graphe des maintenant (pas de lazy-init au premier
// acces) pour que le marqueur/pipe soient utilisables des la ligne de log
// suivante.
var rootPlayer = TestRoot.RootPlayer;
Console.WriteLine(
    $"Graphe de test construit : player='{rootPlayer.Name}' health={rootPlayer.Health} " +
    $"items={rootPlayer.Inventory.Items.Count} identity={TestRoot.StableIdentityOf(rootPlayer)}");

// Nom de pipe surchargeable par variable d'environnement -- necessaire pour
// que les auto-tests de regression (kill abrupt, redemarrage nouveau PID)
// puissent lancer des instances supplementaires isolees sans collision avec
// une instance deja active sur le nom par defaut.
string pipeName = Environment.GetEnvironmentVariable("KILLENGINE_CLR_TEST_TARGET_PIPE_NAME")
    ?? ControlPipeServer.DefaultPipeName;

// Marqueur fichier temp, meme convention que tests/memory_targets/test_target_main.cpp
// (killengine_test_target_addresses.txt) : un futur harness d'auto-test lit ce
// fichier plutot que de deviner PID/pipe name. Suffixe uniquement si le nom
// de pipe est surcharge, pour eviter toute collision entre instances
// isolees sans changer le nom par defaut deja documente.
string markerFileName = pipeName == ControlPipeServer.DefaultPipeName
    ? "killengine_clr_test_target_addresses.txt"
    : $"killengine_clr_test_target_addresses_{pipeName}.txt";
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

// Churn par defaut modeste (10 objets/s) : assez pour provoquer des cycles
// Gen0 reguliers sans consommer un coeur CPU entier au repos. Ajustable a
// chaud via la methode pipe "setChurnRate" (ex: monter a 5000+ pour forcer
// un compactage Gen2 rapidement pendant un test).
var churn = new GcChurnWorker(initialObjectsPerSecond: 10);
churn.Start();

var dispatcher = new MethodDispatcher(churn, requestShutdown: () => cts.Cancel());
var pipeServer = new ControlPipeServer(dispatcher, pipeName);

try
{
    await pipeServer.RunAsync(cts.Token).ConfigureAwait(false);
}
finally
{
    churn.Stop();
    try { File.Delete(markerPath); } catch { /* best-effort */ }
    Console.WriteLine("KillEngineClrTestTarget arrete.");
}

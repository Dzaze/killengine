using System.Diagnostics;
using System.Text.Json.Nodes;

using System.Linq;

namespace KillEngine.ClrInspector.Tests;

/// <summary>
/// Lance KillEngineClrTestTarget.exe et KillEngineClrInspector.exe une seule
/// fois pour toute la classe de tests (cout de demarrage de deux process
/// .NET par test serait inutilement lent) -- partage entre les [Fact] qui
/// n'ont pas besoin d'isolation stricte (ping). Le test central
/// (FullMvpWorkflow...) est volontairement autonome de bout en bout dans une
/// seule methode plutot que reparti sur plusieurs [Fact] : c'est une
/// sequence causale (attacher -> lire -> muter -> GC -> relire), la decouper
/// en tests separes referencant un etat partage mutable serait plus fragile
/// que ca n'apporterait de granularite utile.
/// </summary>
public sealed class TargetAndInspectorFixture : IAsyncLifetime
{
    public ManagedProcessFixture Target { get; private set; } = null!;
    public ManagedProcessFixture Inspector { get; private set; } = null!;

    public async Task InitializeAsync()
    {
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        Target = await ManagedProcessFixture.StartAsync(targetDll, "KillEngineClrTestTargetPipe", TimeSpan.FromSeconds(20));
        Inspector = await ManagedProcessFixture.StartAsync(inspectorDll, "KillEngineClrInspectorPipe", TimeSpan.FromSeconds(20));
    }

    public async Task DisposeAsync()
    {
        if (Inspector is not null) await Inspector.DisposeAsync();
        if (Target is not null) await Target.DisposeAsync();
    }
}

[CollectionDefinition("ClrInspectorEndToEnd", DisableParallelization = true)]
public sealed class ClrInspectorEndToEndCollection : ICollectionFixture<TargetAndInspectorFixture>
{
}

[Collection("ClrInspectorEndToEnd")]
public sealed class EndToEndTests
{
    private const string TargetPipe = "KillEngineClrTestTargetPipe";
    private const string InspectorPipe = "KillEngineClrInspectorPipe";

    private readonly TargetAndInspectorFixture _fixture;

    public EndToEndTests(TargetAndInspectorFixture fixture)
    {
        _fixture = fixture;
    }

    [Fact]
    public async Task Ping_TestTarget_ReturnsPong()
    {
        var result = await PipeClient.CallAsync(TargetPipe, "ping");
        Assert.Contains("KillEngineClrTestTarget", result!.GetValue<string>());
    }

    [Fact]
    public async Task Ping_Inspector_ReturnsPong()
    {
        var result = await PipeClient.CallAsync(InspectorPipe, "ping");
        Assert.Contains("KillEngineClrInspector", result!.GetValue<string>());
    }

    [Fact]
    public async Task Attach_ToNonClrProcess_ReturnsClearError()
    {
        // Cible native volontaire (aucun CLR) -- reproduit exactement le cas
        // qui a invalide l'hypothese "tas .NET managé" sur Solitaire.exe
        // (docs/STRATEGY_ROOM.md, 20/08/2026) : ClrMD doit echouer proprement
        // avec un message explicite, pas planter ni retourner un faux succes.
        // Reutilise l'inspecteur partage du fixture : sans risque pour les
        // autres tests, "attach" reinitialise toujours la session en premier
        // (ClrSession.Attach -> DetachInternal()), donc un attach rate ici
        // n'a aucun effet residuel sur un attach reussi ensuite ailleurs.
        var nativeProcess = Process.Start(new ProcessStartInfo
        {
            FileName = "ping",
            Arguments = "-n 30 127.0.0.1",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        }) ?? throw new InvalidOperationException("Echec de lancement du process natif jetable (ping).");

        try
        {
            var ex = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            {
                await PipeClient.CallAsync(
                    InspectorPipe,
                    "attach",
                    new JsonArray(JsonValue.Create(nativeProcess.Id)));
            });
            Assert.Contains("Aucun CLR", ex.Message, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            if (!nativeProcess.HasExited) nativeProcess.Kill();
            nativeProcess.Dispose();
        }
    }

    [Fact]
    public async Task FullMvpWorkflow_FindsSameLogicalObjectAfterCompactingGc()
    {
        // Isole ce pipe name des autres [Fact] pour ne pas interferer avec
        // Attach_ToNonClrProcess_ReturnsClearError si xUnit les execute dans
        // un ordre non deterministe -- reutilise quand meme l'unique
        // process cible/inspecteur du fixture partage (pas un 3e lancement).
        int targetPid = _fixture.Target.Pid;

        // 1) Detection + attache sur le PID de la cible connue.
        var attachResult = await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(targetPid)));
        Assert.Equal("Core", attachResult!["clrFlavor"]!.GetValue<string>());
        Assert.True(attachResult["clrVersionsFound"]!.GetValue<int>() >= 1);

        // 2) Enumeration du heap -- retrouver le Player connu.
        var foundBefore = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        var playerEntryBefore = Assert.Single(foundBefore!.AsArray());
        string addressBefore = playerEntryBefore!["address"]!.GetValue<string>();

        // 3) Lecture de champs primitifs + reference + cycle auto-reference.
        var objBefore = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(addressBefore)));
        var fieldsBefore = objBefore!["fields"]!;
        Assert.Equal("TestSubject", fieldsBefore["Name"]!.GetValue<string>());
        Assert.Equal(5000, fieldsBefore["Experience"]!.GetValue<long>());
        Assert.True(fieldsBefore["IsAlive"]!.GetValue<bool>());
        Assert.Equal("KillEngine.ClrTestTarget.Inventory", fieldsBefore["Inventory"]!["typeName"]!.GetValue<string>());
        Assert.Equal(addressBefore, fieldsBefore["Self"]!["address"]!.GetValue<string>());

        // 4) Au moins une GC root retrouvee parmi nos types connus.
        var roots = await PipeClient.CallAsync(
            InspectorPipe, "enumerateRoots", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget")));
        Assert.NotEmpty(roots!.AsArray());

        // 5) Sentinelle unique + identite independante (oracle cote cible,
        // RuntimeHelpers.GetHashCode -- calcule DANS le process managé,
        // sans passer par ClrMD) avant le GC.
        int sentinel = Random.Shared.Next(100_000, 999_999);
        await PipeClient.CallAsync(
            TargetPipe, "mutateField", new JsonArray(JsonValue.Create("player.health"), JsonValue.Create(sentinel)));

        var identityBefore = await PipeClient.CallAsync(TargetPipe, "getObjectIdentity");
        int stableIdentity = identityBefore!["player"]!.GetValue<int>();

        var objBeforeSentinel = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(addressBefore)));
        Assert.Equal(sentinel, objBeforeSentinel!["fields"]!["Health"]!.GetValue<int>());

        // 6) Forcer un GC Gen2 compactant (churn eleve pour maximiser la
        // probabilite d'un vrai deplacement, pas garanti mais tres probable).
        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(50_000)));
        var gcResult = await PipeClient.CallAsync(TargetPipe, "forceGC");
        Assert.True(gcResult!["gen2After"]!.GetValue<int>() > gcResult["gen2Before"]!.GetValue<int>());
        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));

        // 7) Invalider le cache ClrMD puis re-resoudre l'objet -- l'adresse
        // peut avoir change (deplacement par le compactage), le contenu logique doit rester le meme.
        await PipeClient.CallAsync(InspectorPipe, "flushCachedData");
        var foundAfter = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        var playerEntryAfter = Assert.Single(foundAfter!.AsArray());
        string addressAfter = playerEntryAfter!["address"]!.GetValue<string>();

        var objAfter = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(addressAfter)));
        var fieldsAfter = objAfter!["fields"]!;

        // La preuve centrale du MVP : meme sentinelle retrouvee (que
        // l'adresse ait change ou non -- un compactage Gen2 ne garantit pas
        // toujours un deplacement de CET objet precis, mais le mecanisme de
        // re-resolution doit fonctionner dans les deux cas).
        Assert.Equal(sentinel, fieldsAfter["Health"]!.GetValue<int>());
        Assert.Equal("TestSubject", fieldsAfter["Name"]!.GetValue<string>());
        Assert.Equal(addressAfter, fieldsAfter["Self"]!["address"]!.GetValue<string>());

        // 8) Cross-check independant : l'identite stable cote cible (avant
        // le GC) est identique a celle d'apres -- deuxieme preuve, calculee
        // par un mecanisme totalement different (sync block hashcode
        // managé) de la premiere (comparaison de valeur de champ via ClrMD).
        var identityAfter = await PipeClient.CallAsync(TargetPipe, "getObjectIdentity");
        Assert.Equal(stableIdentity, identityAfter!["player"]!.GetValue<int>());
    }

    [Fact]
    public async Task MultipleGcCycles_SameObjectSurvivesEachCycleWithConsistentIdentity()
    {
        // Renforce FullMvpWorkflow (un seul GC) : le meme objet doit rester
        // retrouvable et coherent apres PLUSIEURS compactages successifs,
        // pas seulement le premier -- garde-fou contre une regression qui ne
        // se manifesterait qu'au 2e ou 3e cycle (etat cache pas invalide
        // correctement, par exemple).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        string? currentAddress = null;
        for (int cycle = 1; cycle <= 3; cycle++)
        {
            int sentinel = 1_000_000 + cycle;
            await PipeClient.CallAsync(
                TargetPipe, "mutateField", new JsonArray(JsonValue.Create("player.health"), JsonValue.Create(sentinel)));
            var identityBefore = await PipeClient.CallAsync(TargetPipe, "getObjectIdentity");

            await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(50_000)));
            await PipeClient.CallAsync(TargetPipe, "forceGC");
            await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));
            await PipeClient.CallAsync(InspectorPipe, "flushCachedData");

            var found = await PipeClient.CallAsync(
                InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
            var entry = Assert.Single(found!.AsArray());
            currentAddress = entry!["address"]!.GetValue<string>();

            var obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(currentAddress)));
            Assert.Equal(sentinel, obj!["fields"]!["Health"]!.GetValue<int>());

            var identityAfter = await PipeClient.CallAsync(TargetPipe, "getObjectIdentity");
            Assert.Equal(identityBefore!["player"]!.GetValue<int>(), identityAfter!["player"]!.GetValue<int>());
        }
    }

    [Fact]
    public async Task MultipleItemObjects_HaveDistinctAddressesAndKnownFields()
    {
        // Le graphe de test contient 3 Item connus (Sword/Shield/Potion,
        // ObjectGraph.cs) -- verifie que findObjectsByType les distingue
        // correctement les uns des autres (pas la meme adresse rapportee
        // 3 fois, pas de champs melanges entre instances).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Item")));
        var entries = found!.AsArray().Where(e => e!["typeName"]!.GetValue<string>() == "KillEngine.ClrTestTarget.Item").ToList();
        Assert.Equal(3, entries.Count);

        var addresses = new HashSet<string>();
        var names = new HashSet<string>();
        foreach (var entry in entries)
        {
            string address = entry!["address"]!.GetValue<string>();
            Assert.True(addresses.Add(address), $"Adresse dupliquee : {address}");

            var obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(address)));
            string name = obj!["fields"]!["Name"]!.GetValue<string>();
            names.Add(name);
        }

        Assert.Equal(3, names.Count);
        Assert.Equal(new[] { "Potion", "Shield", "Sword" }, names.OrderBy(n => n));
    }

    [Fact]
    public async Task DroppedObject_IsCollected_WhileUnrelatedObjectSurvives()
    {
        // Distingue explicitement "deplace par le GC" (Player, deja couvert
        // par FullMvpWorkflow) de "reellement devenu inatteignable et
        // collecte" (DisposableProbe) -- les deux dans le meme test pour
        // que la difference de comportement soit la preuve, pas juste
        // une absence isolee qui pourrait aussi bien etre un bug de filtre.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var spawnResult = await PipeClient.CallAsync(
            TargetPipe, "spawnDisposable", new JsonArray(JsonValue.Create("regression-collect-test")));
        int spawnedId = spawnResult!["id"]!.GetValue<int>();

        var foundBefore = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.DisposableProbe")));
        var probeEntry = Assert.Single(foundBefore!.AsArray());
        var probeObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(probeEntry!["address"]!.GetValue<string>())));
        Assert.Equal(spawnedId, probeObj!["fields"]!["Id"]!.GetValue<int>());

        await PipeClient.CallAsync(TargetPipe, "dropDisposable");
        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(50_000)));
        await PipeClient.CallAsync(TargetPipe, "forceGC");
        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));
        await PipeClient.CallAsync(InspectorPipe, "flushCachedData");

        var foundAfter = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.DisposableProbe")));
        Assert.Empty(foundAfter!.AsArray());

        // Contraste : le Player, lui, reste bien retrouvable (potentiellement
        // deplace, jamais collecte -- il est toujours reference par
        // TestRoot.RootPlayer). Confirme que l'absence ci-dessus est une
        // vraie collecte, pas un `findObjectsByType` casse qui ne trouverait
        // plus rien du tout.
        var playerAfter = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        Assert.NotEmpty(playerAfter!.AsArray());
    }

    [Fact]
    public async Task AbruptTargetTermination_InspectorStaysAliveAndReportsCleanError()
    {
        // Processus isoles (pas le fixture partage) : ce test tue son
        // propre target, ne doit avoir aucun effet sur les autres tests de
        // la classe. Noms de pipe surcharges (KILLENGINE_CLR_*_PIPE_NAME)
        // pour ne jamais collisionner avec l'instance par defaut deja active.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_AbruptKillTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_AbruptKillTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        var attachResult = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));
        Assert.Equal("Core", attachResult!["clrFlavor"]!.GetValue<string>());

        // Terminaison brutale (pas "shutdown" gracieux) pendant qu'une
        // session ClrMD est active sur ce PID.
        isolatedTarget.Process.Kill(entireProcessTree: true);
        Assert.True(isolatedTarget.Process.WaitForExit(5000), "Le process cible n'a pas termine apres Kill().");

        // Consequence attendue et volontaire d'un kill brutal (pas un bug) :
        // le `finally` du process cible (qui supprime son propre marqueur)
        // n'a jamais pu s'executer -- nettoyage best-effort ici pour ne pas
        // laisser un fichier orphelin a chaque execution de ce test.
        try
        {
            File.Delete(Path.Combine(Path.GetTempPath(), $"killengine_clr_test_target_addresses_{isolatedTargetPipe}.txt"));
        }
        catch { /* best-effort */ }

        // L'inspecteur doit rester vivant et reactif -- un crash ici serait
        // le vrai risque de stabilite que docs/KILLENGINE_CLR_INSPECTOR_SPEC.md
        // signale explicitement (attache passive sur process qui disparait).
        var pingResult = await PipeClient.CallAsync(isolatedInspectorPipe, "ping");
        Assert.Contains("KillEngineClrInspector", pingResult!.GetValue<string>());

        // Un appel qui a besoin de relire la cible morte doit echouer
        // proprement (erreur JSON-RPC catchable), jamais planter le pipe ni
        // rester bloque indefiniment.
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                isolatedInspectorPipe, "findObjectsByType",
                new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget")),
                connectTimeoutMs: 5000);
        });

        // Deuxieme ping apres l'echec ci-dessus : confirme que l'exception
        // n'a pas laisse le serveur pipe dans un etat casse pour la requete suivante.
        var pingAfterFailure = await PipeClient.CallAsync(isolatedInspectorPipe, "ping");
        Assert.Contains("KillEngineClrInspector", pingAfterFailure!.GetValue<string>());
    }

    [Fact]
    public async Task RestartWithNewPid_InspectorAttachesCleanlyToFreshTarget()
    {
        // Simule un redemarrage de la cible (nouveau PID) pendant qu'un
        // inspecteur existe deja -- verifie l'absence d'etat residuel de la
        // premiere session (ClrSession.Attach() reinitialise toujours via
        // DetachInternal() avant de re-attacher, voir ClrSession.cs).
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_RestartTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_RestartTest";
        var targetEnv = new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe };

        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        int firstPid;
        await using (var firstTarget = await ManagedProcessFixture.StartAsync(targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20), targetEnv))
        {
            firstPid = firstTarget.Pid;
            var attach1 = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(firstPid)));
            Assert.Equal(firstPid, attach1!["pid"]!.GetValue<int>());
            // Arret gracieux complet (attente WaitForExit dans DisposeAsync) avant
            // de liberer le nom de pipe pour la deuxieme instance.
        }

        await using var secondTarget = await ManagedProcessFixture.StartAsync(targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20), targetEnv);
        Assert.NotEqual(firstPid, secondTarget.Pid);

        var attach2 = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(secondTarget.Pid)));
        Assert.Equal(secondTarget.Pid, attach2!["pid"]!.GetValue<int>());
        Assert.Equal("Core", attach2["clrFlavor"]!.GetValue<string>());

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        Assert.Single(found!.AsArray());
    }
}

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

        string inventoryAddress = fieldsBefore["Inventory"]!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var inventoryFields = inventoryObj!["fields"]!;
        var itemsCollection = Field(inventoryFields, "Items")!["collection"]!;
        Assert.Equal("list", itemsCollection["kind"]!.GetValue<string>());
        Assert.Equal(3, itemsCollection["count"]!.GetValue<int>());
        var firstItemRef = itemsCollection["items"]![0]!;
        Assert.Equal("KillEngine.ClrTestTarget.Item", firstItemRef["typeName"]!.GetValue<string>());
        string firstItemAddress = firstItemRef["address"]!.GetValue<string>();
        var firstItemObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(firstItemAddress)));
        Assert.Equal("Sword", firstItemObj!["fields"]!["Name"]!.GetValue<string>());

        var quickSlots = Field(inventoryFields, "QuickSlots")!["collection"]!;
        Assert.Equal("array", quickSlots["kind"]!.GetValue<string>());
        Assert.Equal(4, quickSlots["count"]!.GetValue<int>());
        Assert.Equal("KillEngine.ClrTestTarget.Item", quickSlots["items"]![1]!["typeName"]!.GetValue<string>());

        var currencies = Field(inventoryFields, "Currencies")!["collection"]!;
        Assert.Equal("dictionary", currencies["kind"]!.GetValue<string>());
        Assert.Equal(2, currencies["count"]!.GetValue<int>());
        Assert.Contains(currencies["entries"]!.AsArray(), entry =>
            entry!["key"]!.GetValue<string>() == "gold" && entry["value"]!.GetValue<int>() == 4125);

        var customItems = Field(inventoryFields, "CustomItems")!["collection"]!;
        Assert.Equal("custom_field_backed", customItems["kind"]!.GetValue<string>());
        Assert.Equal(2, customItems["collection"]!["count"]!.GetValue<int>());
        Assert.Equal("KillEngine.ClrTestTarget.Item", customItems["collection"]!["items"]![0]!["typeName"]!.GetValue<string>());

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
    public async Task WritePrimitiveField_UpdatesManagedObjectAndReportsFieldAddress()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        var playerEntry = Assert.Single(found!.AsArray());
        string playerAddress = playerEntry!["address"]!.GetValue<string>();

        int sentinel = Random.Shared.Next(2_000_000, 2_999_999);
        var writeResult = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitiveField",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Health"),
                JsonValue.Create(sentinel)));

        Assert.Equal("Health", writeResult!["fieldName"]!.GetValue<string>());
        Assert.Equal("Int32", writeResult["elementType"]!.GetValue<string>());
        Assert.Equal(4, writeResult["bytesWritten"]!.GetValue<int>());
        Assert.True(writeResult["verified"]!.GetValue<bool>());
        Assert.StartsWith("0x", writeResult["fieldAddress"]!.GetValue<string>(), StringComparison.OrdinalIgnoreCase);
        Assert.Equal(sentinel, writeResult["value"]!.GetValue<int>());

        var obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        Assert.Equal(sentinel, obj!["fields"]!["Health"]!.GetValue<int>());

        var details = obj["fieldDetails"]!.AsArray();
        var healthDetail = Assert.Single(details, field => field!["name"]!.GetValue<string>() == "Health");
        Assert.True(healthDetail!["writable"]!.GetValue<bool>());
        Assert.Equal(writeResult["fieldAddress"]!.GetValue<string>(), healthDetail["address"]!.GetValue<string>());

        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(sentinel, status!["player"]!["health"]!.GetValue<int>());
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesNestedReferencesAndListItems()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        var playerEntry = Assert.Single(found!.AsArray());
        string playerAddress = playerEntry!["address"]!.GetValue<string>();

        int selfHealth = Random.Shared.Next(3_000_000, 3_499_999);
        var selfWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Self.Health"),
                JsonValue.Create(selfHealth)));

        Assert.Equal("Self.Health", selfWrite!["path"]!.GetValue<string>());
        Assert.Equal("Health", selfWrite["fieldName"]!.GetValue<string>());
        Assert.True(selfWrite["verified"]!.GetValue<bool>());
        Assert.Equal(selfHealth, selfWrite["value"]!.GetValue<int>());

        var selfStatus = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(selfHealth, selfStatus!["player"]!["health"]!.GetValue<int>());

        int itemValue = Random.Shared.Next(3_500_000, 3_999_999);
        var itemWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Inventory.Items[0].Value"),
                JsonValue.Create(itemValue)));

        Assert.Equal("Inventory.Items[0].Value", itemWrite!["path"]!.GetValue<string>());
        Assert.Equal("Value", itemWrite["fieldName"]!.GetValue<string>());
        Assert.Equal("KillEngine.ClrTestTarget.Item", itemWrite["typeName"]!.GetValue<string>());
        Assert.True(itemWrite["verified"]!.GetValue<bool>());
        Assert.Equal(itemValue, itemWrite["value"]!.GetValue<int>());

        var itemStatus = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(itemValue, itemStatus!["player"]!["firstItemValue"]!.GetValue<int>());
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesStringReferenceStructAndDictionaryValues()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();

        var stringWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Name"),
                JsonValue.Create("NoviceGuide")));
        Assert.Equal("string_in_place_same_length", stringWrite!["mode"]!.GetValue<string>());
        Assert.True(stringWrite["verified"]!.GetValue<bool>());

        var structWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Stats.Rank"),
                JsonValue.Create(42)));
        Assert.Equal("Stats.Rank", structWrite!["path"]!.GetValue<string>());
        Assert.True(structWrite["verified"]!.GetValue<bool>());

        var dictionaryWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Inventory.Currencies[gold]"),
                JsonValue.Create(7777)));
        Assert.Equal("Inventory.Currencies[gold]", dictionaryWrite!["path"]!.GetValue<string>());
        Assert.True(dictionaryWrite["verified"]!.GetValue<bool>());

        var items = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Item")));
        string shieldAddress = "";
        foreach (var entry in items!.AsArray())
        {
            if (entry!["typeName"]!.GetValue<string>() != "KillEngine.ClrTestTarget.Item")
            {
                continue;
            }
            string address = entry!["address"]!.GetValue<string>();
            var item = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(address)));
            if (item!["fields"]!["Name"]!.GetValue<string>() == "Shield")
            {
                shieldAddress = address;
                break;
            }
        }
        Assert.False(string.IsNullOrWhiteSpace(shieldAddress));

        var referenceWrite = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Inventory.QuickSlots[2]"),
                JsonValue.Create(shieldAddress)));
        Assert.Equal("Inventory.QuickSlots[2]", referenceWrite!["path"]!.GetValue<string>());
        Assert.True(referenceWrite["verified"]!.GetValue<bool>());

        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal("NoviceGuide", status!["player"]!["name"]!.GetValue<string>());
        Assert.Equal(42, status["player"]!["statsRank"]!.GetValue<int>());
        Assert.Equal(7777, status["player"]!["gold"]!.GetValue<int>());
        Assert.Equal("Shield", status["player"]!["quickSlot2Name"]!.GetValue<string>());

        var restoreName = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Name"),
                JsonValue.Create("TestSubject")));
        Assert.True(restoreName!["verified"]!.GetValue<bool>());
    }

    [Fact]
    public async Task WritePrimitivePathBatch_RollsBackAlreadyAppliedWritesOnFailure()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();

        await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Health"),
                JsonValue.Create(1234)));

        var batch = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePathBatch",
            new JsonArray(
                JsonValue.Create(playerAddress),
                new JsonArray(
                    new JsonObject { ["path"] = "Health", ["value"] = "4321" },
                    new JsonObject { ["path"] = "Stats.Missing", ["value"] = "99" })));

        Assert.False(batch!["success"]!.GetValue<bool>());
        Assert.Equal(1, batch["appliedBeforeFailure"]!.GetValue<int>());
        Assert.True(batch["rolledBack"]!.GetValue<bool>());

        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(1234, status!["player"]!["health"]!.GetValue<int>());
    }

    [Fact]
    public async Task FieldValueLocator_RefindsObjectAfterCompactingGc()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundBefore = await PipeClient.CallAsync(
            InspectorPipe,
            "findObjectsByFieldValue",
            new JsonArray(
                JsonValue.Create("KillEngine.ClrTestTarget.Player"),
                JsonValue.Create("Name"),
                JsonValue.Create("TestSubject"),
                JsonValue.Create(5)));
        Assert.True(foundBefore!["success"]!.GetValue<bool>());
        var matchBefore = Assert.Single(foundBefore["matches"]!.AsArray());
        string addressBefore = matchBefore!["address"]!.GetValue<string>();

        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(50_000)));
        await PipeClient.CallAsync(TargetPipe, "forceGC");
        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));
        await PipeClient.CallAsync(InspectorPipe, "flushCachedData");

        var foundAfter = await PipeClient.CallAsync(
            InspectorPipe,
            "findObjectsByFieldValue",
            new JsonArray(
                JsonValue.Create("KillEngine.ClrTestTarget.Player"),
                JsonValue.Create("Name"),
                JsonValue.Create("TestSubject"),
                JsonValue.Create(5)));
        Assert.True(foundAfter!["success"]!.GetValue<bool>());
        var matchAfter = Assert.Single(foundAfter["matches"]!.AsArray());
        string addressAfter = matchAfter!["address"]!.GetValue<string>();

        var objAfter = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(addressAfter)));
        Assert.Equal("TestSubject", objAfter!["fields"]!["Name"]!.GetValue<string>());
        Assert.Equal(addressAfter, objAfter["fields"]!["Self"]!["address"]!.GetValue<string>());
        Assert.False(string.IsNullOrWhiteSpace(addressBefore));
    }

    [Fact]
    public async Task FieldValueLocator_FindsPrimitiveFieldMatches()
    {
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe,
            "findObjectsByFieldValue",
            new JsonArray(
                JsonValue.Create("KillEngine.ClrTestTarget.Item"),
                JsonValue.Create("Value"),
                JsonValue.Create("150"),
                JsonValue.Create(10)));

        Assert.True(found!["success"]!.GetValue<bool>());
        var match = Assert.Single(found["matches"]!.AsArray());
        Assert.Equal("Value", match!["identityField"]!.GetValue<string>());
        Assert.Equal(150, match["identityValue"]!.GetValue<int>());
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

    [Fact]
    public async Task ResolveInstanceMethodAddress_NeverCalledSetter_ReturnsClearJitError()
    {
        // NeverCalledStat (ObjectGraph.cs) n'a aucun warmup dans BuildGraph --
        // son set_NeverCalledStat n'est donc jamais JITte par le process
        // cible. ResolveInstanceMethodAddress doit renvoyer le message clair
        // documente (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md), pas tenter de
        // forcer une compilation JIT (impossible sans ICorDebug, hors scope).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var ex = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "resolveInstanceMethodAddress",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("NeverCalledStat")));
        });
        Assert.Contains("jamais ete appele", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_StaticMethod_IsRejected()
    {
        // Garde-fou "instance uniquement" (this en RCX n'a pas de sens pour
        // un appel static) : Player.StaticProbe (ObjectGraph.cs) est une
        // methode statique dediee a ce test -- ResolveInstanceMethodAddress
        // doit la rejeter explicitement, jamais tenter de la resoudre comme
        // si elle prenait un "this" implicite.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var ex = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "resolveInstanceMethodAddress",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("StaticProbe")));
        });
        Assert.Contains("statique", ex.Message, StringComparison.OrdinalIgnoreCase);

        // Cross-check : Vitality (instance normale) n'est PAS rejetee a tort
        // par le meme garde-fou -- confirme que le test negatif ci-dessus ne
        // masque pas un faux-positif qui rejetterait tout.
        var resolved = await PipeClient.CallAsync(
            InspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Vitality")));
        Assert.False(resolved!["isStatic"]!.GetValue<bool>());
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_InvokesRealSetterAndProducesSideEffect()
    {
        // Preuve centrale du chantier "appel de setter reel" : resout
        // l'adresse native deja JITtee de Player.set_Vitality via ClrMD
        // (ResolveInstanceMethodAddress), puis appelle REELLEMENT ce setter
        // par injection shellcode (NativeSetterInvoker -- meme technique
        // exacte que apps/desktop/application_controller.cpp::
        // callClrInstanceMethod cote natif, voir ce fichier). La preuve que
        // le VRAI setter a tourne (pas un raccourci d'ecriture memoire brute
        // du champ backing) : la valeur ecrite est CLAMPEE a VitalityMax et
        // un compteur de changements SEPARE (_vitalityChangeCount)
        // s'incremente -- une ecriture directe de _vitality ne produirait
        // jamais ces deux effets de bord. Verifie via le pipe de controle de
        // la cible (oracle independant de ClrMD), meme discipline que
        // WritePrimitiveField_UpdatesManagedObjectAndReportsFieldAddress.
        //
        // Process isole (pas le fixture partage) : ce test amene
        // deliberement IsAlive a false en fin de sequence, effet qui ne doit
        // affecter aucun autre test partageant le Player du fixture commun.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_SetterCallTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_SetterCallTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        var attachResult = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));
        Assert.Equal("Core", attachResult!["clrFlavor"]!.GetValue<string>());

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        ulong objectAddress = ParseHex(playerAddress);

        // 1) Resolution reelle de l'adresse native via ClrMD (Vitality ->
        // fallback automatique vers set_Vitality, deja JITte par le warmup
        // unique dans TestRoot.BuildGraph, ObjectGraph.cs).
        var resolved = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Vitality")));
        Assert.True(resolved!["success"]!.GetValue<bool>());
        Assert.Equal("set_Vitality", resolved["methodName"]!.GetValue<string>());
        Assert.Equal("Int32", resolved["parameterType"]!.GetValue<string>());
        Assert.False(resolved["isStatic"]!.GetValue<bool>());
        string nativeCodeAddressHex = resolved["nativeCodeAddress"]!.GetValue<string>();
        Assert.StartsWith("0x", nativeCodeAddressHex, StringComparison.OrdinalIgnoreCase);
        ulong nativeCodeAddress = ParseHex(nativeCodeAddressHex);
        Assert.NotEqual(0UL, nativeCodeAddress);

        var statusBefore = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        int changeCountBefore = statusBefore!["player"]!["vitalityChangeCount"]!.GetValue<int>();

        // 2) Appel reel du setter (via shellcode) avec une valeur qui DOIT
        // etre clampee -- 1500 > Player.VitalityMax (999). Une ecriture
        // memoire brute du champ backing accepterait 1500 tel quel ; le
        // vrai setter, lui, la ramene a 999.
        bool completed = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: unchecked((ulong)(long)1500), nativeCodeAddress);
        Assert.True(completed, "Le thread distant n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres l'appel shellcode du setter.");

        var statusAfterClamp = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(999, statusAfterClamp!["player"]!["vitality"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 1, statusAfterClamp["player"]!["vitalityChangeCount"]!.GetValue<int>());
        Assert.True(statusAfterClamp["player"]!["isAlive"]!.GetValue<bool>());

        // Cross-verification independante de l'oracle cote cible : relecture
        // ClrMD directe des champs backing (memes valeurs attendues).
        var objAfterClamp = await PipeClient.CallAsync(isolatedInspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        Assert.Equal(999, objAfterClamp!["fields"]!["_vitality"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 1, objAfterClamp["fields"]!["_vitalityChangeCount"]!.GetValue<int>());

        // 3) Deuxieme appel reel avec 0 -- doit declencher le deuxieme effet
        // de bord metier (IsAlive => false), preuve supplementaire que la
        // logique du VRAI setter s'execute (une ecriture brute de _vitality
        // ne toucherait jamais IsAlive).
        bool completedZero = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: 0UL, nativeCodeAddress);
        Assert.True(completedZero, "Le thread distant (appel 0) n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres le deuxieme appel shellcode du setter.");

        var statusAfterZero = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(0, statusAfterZero!["player"]!["vitality"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 2, statusAfterZero["player"]!["vitalityChangeCount"]!.GetValue<int>());
        Assert.False(statusAfterZero["player"]!["isAlive"]!.GetValue<bool>());
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithStructParameter_PacksFieldsIntoRdxAndProducesSideEffect()
    {
        // Chantier "setters a parametre struct" (docs/POWER_UP_ROADMAP.md
        // candidat #8, extension listee "non couverte a ce jour") :
        // Player.set_Waypoint prend un parametre STRUCT (Coordinates, 8
        // octets, deux champs Int32 X/Y) -- ni primitif ni type reference.
        // La convention d'appel x64 Windows passe un struct de 1/2/4/8 octets
        // PAR VALEUR dans un unique registre (RDX) : les octets bruts du
        // struct (meme agencement memoire que quand on le lit via ClrMD,
        // X a l'offset 0, Y a l'offset 4) sont charges tels quels, EXACTEMENT
        // le meme mecanisme shellcode que pour un parametre primitif entier
        // -- prouve ici en composant l'immediate a la main a partir des
        // offsets/tailles renvoyes par resolveInstanceMethodAddress, sans
        // aucun changement necessaire a NativeSetterInvoker/au shellcode
        // existant. Preuve que le VRAI setter tourne (pas une ecriture
        // memoire brute) : clamp a [0,100] sur les DEUX champs et un compteur
        // de changements SEPARE, meme discipline que Vitality/Vigor.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_WaypointSetterTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_WaypointSetterTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        ulong objectAddress = ParseHex(playerAddress);

        // 1) Resolution reelle -- verifie que le struct est bien reconnu et
        // decrit (taille + champs avec offsets), pas seulement accepte a
        // l'aveugle.
        var resolved = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Waypoint")));
        Assert.True(resolved!["success"]!.GetValue<bool>());
        Assert.Equal("set_Waypoint", resolved["methodName"]!.GetValue<string>());
        Assert.Contains("Coordinates", resolved["parameterType"]!.GetValue<string>());
        Assert.True(resolved["parameterIsStruct"]!.GetValue<bool>());
        Assert.False(resolved["parameterIsReferenceType"]!.GetValue<bool>());
        Assert.Equal(8, resolved["parameterStructSize"]!.GetValue<int>());
        var fields = resolved["parameterStructFields"]!.AsArray()
            .ToDictionary(f => f!["name"]!.GetValue<string>(), f => f);
        Assert.Equal(0, fields["X"]!["offset"]!.GetValue<int>());
        Assert.Equal(4, fields["X"]!["size"]!.GetValue<int>());
        Assert.Equal(4, fields["Y"]!["offset"]!.GetValue<int>());
        Assert.Equal(4, fields["Y"]!["size"]!.GetValue<int>());
        ulong nativeCodeAddress = ParseHex(resolved["nativeCodeAddress"]!.GetValue<string>());
        Assert.NotEqual(0UL, nativeCodeAddress);

        // 2) Compose l'immediate RDX a la main depuis les offsets resolus --
        // exactement ce que ApplicationController::callClrInstanceMethod doit
        // faire cote natif pour un parametre struct (aucune logique
        // supplementaire cachee cote helper .NET, la resolution ne fait que
        // DECRIRE le layout).
        static ulong PackStruct(IReadOnlyDictionary<string, JsonNode?> offsets, params (string Name, int Value)[] values)
        {
            Span<byte> buffer = stackalloc byte[8];
            foreach (var (name, value) in values)
            {
                int offset = offsets[name]!["offset"]!.GetValue<int>();
                BitConverter.GetBytes(value).CopyTo(buffer[offset..]);
            }
            return BitConverter.ToUInt64(buffer);
        }

        ulong immediate = PackStruct(fields!, ("X", 55), ("Y", 66));

        var statusBefore = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        int changeCountBefore = statusBefore!["player"]!["waypointChangeCount"]!.GetValue<int>();

        bool completed = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: immediate, nativeCodeAddress);
        Assert.True(completed, "Le thread distant n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres l'appel shellcode du setter struct.");

        var statusAfter = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(55, statusAfter!["player"]!["waypointX"]!.GetValue<int>());
        Assert.Equal(66, statusAfter["player"]!["waypointY"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 1, statusAfter["player"]!["waypointChangeCount"]!.GetValue<int>());

        // 3) Deuxieme appel avec des valeurs HORS BORNES sur les DEUX champs
        // -- doit etre clampe a [0,100], preuve que le VRAI setter tourne
        // (une ecriture memoire brute du champ backing accepterait 500/-10
        // tels quels).
        ulong outOfRangeImmediate = PackStruct(fields!, ("X", 500), ("Y", -10));
        bool completedClamp = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: outOfRangeImmediate, nativeCodeAddress);
        Assert.True(completedClamp, "Le thread distant (appel hors bornes) n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres le deuxieme appel shellcode du setter struct.");

        var statusAfterClamp = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(100, statusAfterClamp!["player"]!["waypointX"]!.GetValue<int>());
        Assert.Equal(0, statusAfterClamp["player"]!["waypointY"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 2, statusAfterClamp["player"]!["waypointChangeCount"]!.GetValue<int>());

        // Cross-verification independante de l'oracle cote cible : relecture
        // ClrMD directe du champ backing.
        var objAfterClamp = await PipeClient.CallAsync(isolatedInspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        var waypointBackingField = objAfterClamp!["fields"]!["_waypoint"]!["fields"]!;
        Assert.Equal(100, waypointBackingField["X"]!.GetValue<int>());
        Assert.Equal(0, waypointBackingField["Y"]!.GetValue<int>());
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithLargeStructParameter_PassesHiddenPointerAndProducesSideEffect()
    {
        // Extension "setters a parametre struct de 9+ octets" (docs/
        // POWER_UP_ROADMAP.md, dernier point du chantier "setters a
        // parametre struct" PHASE 76) : Player.set_Territory prend un
        // parametre STRUCT (Region, 12 octets -- 3 x Int32, delibere ni
        // puissance de 2 ni multiple de 8) trop grand pour tenir dans RDX
        // seul. La convention d'appel x64 Windows le passe alors PAR
        // POINTEUR CACHE vers une copie fournie par l'appelant -- AUCUNE
        // variante "paire de registres" sur cette ABI (contrairement a
        // System V/Linux), confirme ici par l'execution reelle plutot que
        // suppose depuis la doc Microsoft seule. NativeSetterInvoker ecrit
        // les octets du struct A LA SUITE du shellcode dans le meme buffer
        // injecte et charge RDX via un LEA RIP-relatif -- EXACTEMENT le
        // mecanisme que ClrInspectorBridge::buildCallInstanceMethodShellcode
        // reproduit cote natif. Preuve que le VRAI setter tourne (pas une
        // ecriture memoire brute) : clamp sur les TROIS champs et un
        // compteur de changements SEPARE, meme discipline que Waypoint.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_TerritorySetterTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_TerritorySetterTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        ulong objectAddress = ParseHex(playerAddress);

        // 1) Resolution reelle -- verifie que le struct de 12 octets est
        // reconnu et decrit avec parameterStructPassedByRef=true (le
        // discriminant que ClrInspectorBridge::callClrInstanceMethod utilise
        // pour choisir le mecanisme pointeur plutot que registre).
        var resolved = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Territory")));
        Assert.True(resolved!["success"]!.GetValue<bool>());
        Assert.Equal("set_Territory", resolved["methodName"]!.GetValue<string>());
        Assert.Contains("Region", resolved["parameterType"]!.GetValue<string>());
        Assert.True(resolved["parameterIsStruct"]!.GetValue<bool>());
        Assert.False(resolved["parameterIsReferenceType"]!.GetValue<bool>());
        Assert.Equal(12, resolved["parameterStructSize"]!.GetValue<int>());
        Assert.True(resolved["parameterStructPassedByRef"]!.GetValue<bool>());
        var fields = resolved["parameterStructFields"]!.AsArray()
            .ToDictionary(f => f!["name"]!.GetValue<string>(), f => f);
        Assert.Equal(0, fields["X"]!["offset"]!.GetValue<int>());
        Assert.Equal(4, fields["Y"]!["offset"]!.GetValue<int>());
        Assert.Equal(8, fields["Width"]!["offset"]!.GetValue<int>());
        ulong nativeCodeAddress = ParseHex(resolved["nativeCodeAddress"]!.GetValue<string>());
        Assert.NotEqual(0UL, nativeCodeAddress);

        // 2) Compose les 12 octets bruts a la main depuis les offsets
        // resolus -- exactement ce que ClrInspectorBridge::
        // encodeStructParameterBytes doit produire cote natif.
        static byte[] PackStructBytes(IReadOnlyDictionary<string, JsonNode?> offsets, int totalSize, params (string Name, int Value)[] values)
        {
            byte[] buffer = new byte[totalSize];
            foreach (var (name, value) in values)
            {
                int offset = offsets[name]!["offset"]!.GetValue<int>();
                BitConverter.GetBytes(value).CopyTo(buffer, offset);
            }
            return buffer;
        }

        byte[] structBytes = PackStructBytes(fields!, 12, ("X", 55), ("Y", 66), ("Width", 77));

        var statusBefore = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        int changeCountBefore = statusBefore!["player"]!["territoryChangeCount"]!.GetValue<int>();

        bool completed = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: 0, nativeCodeAddress,
            structByRefBytes: structBytes);
        Assert.True(completed, "Le thread distant n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres l'appel shellcode du setter struct >8 octets.");

        var statusAfter = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(55, statusAfter!["player"]!["territoryX"]!.GetValue<int>());
        Assert.Equal(66, statusAfter["player"]!["territoryY"]!.GetValue<int>());
        Assert.Equal(77, statusAfter["player"]!["territoryWidth"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 1, statusAfter["player"]!["territoryChangeCount"]!.GetValue<int>());

        // 3) Deuxieme appel avec des valeurs HORS BORNES sur les TROIS
        // champs -- doit etre clampe (X/Y -> [0,1000], Width -> [1,500]),
        // preuve que le VRAI setter tourne (une ecriture memoire brute du
        // champ backing accepterait 5000/-10/900 tels quels). Confirme aussi
        // que le buffer struct temporaire (adresse choisie dynamiquement par
        // VirtualAllocEx a CHAQUE appel) fonctionne de facon repetable, pas
        // juste au premier essai.
        byte[] outOfRangeBytes = PackStructBytes(fields!, 12, ("X", 5000), ("Y", -10), ("Width", 900));
        bool completedClamp = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: 0, nativeCodeAddress,
            structByRefBytes: outOfRangeBytes);
        Assert.True(completedClamp, "Le thread distant (appel hors bornes) n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres le deuxieme appel shellcode du setter struct >8 octets.");

        var statusAfterClamp = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(1000, statusAfterClamp!["player"]!["territoryX"]!.GetValue<int>());
        Assert.Equal(0, statusAfterClamp["player"]!["territoryY"]!.GetValue<int>());
        Assert.Equal(500, statusAfterClamp["player"]!["territoryWidth"]!.GetValue<int>());
        Assert.Equal(changeCountBefore + 2, statusAfterClamp["player"]!["territoryChangeCount"]!.GetValue<int>());

        // Cross-verification independante de l'oracle cote cible : relecture
        // ClrMD directe du champ backing.
        var objAfterClampTerritory = await PipeClient.CallAsync(isolatedInspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        var territoryBackingField = objAfterClampTerritory!["fields"]!["_territory"]!["fields"]!;
        Assert.Equal(1000, territoryBackingField["X"]!.GetValue<int>());
        Assert.Equal(0, territoryBackingField["Y"]!.GetValue<int>());
        Assert.Equal(500, territoryBackingField["Width"]!.GetValue<int>());
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithDoubleParameter_LoadsXmm1AndProducesSideEffect()
    {
        // PHASE 59 -- chantier "setters float/double" : Player.set_Vigor
        // prend un parametre DOUBLE, pas un entier -- la convention d'appel
        // x64 Windows le passe en XMM1, pas RDX. Preuve que le shellcode
        // charge reellement XMM1 (pas un raccourci d'ecriture memoire brute
        // du champ backing _vigor) : la valeur ecrite (250.0) DOIT etre
        // clampee a VigorMax (100.0) et un compteur de changements SEPARE
        // (_vigorChangeCount) s'incremente -- meme discipline que le test
        // Vitality (int, RDX) plus haut. Process isole pour ne pas affecter
        // les autres tests partageant le Player du fixture commun.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_VigorSetterTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_VigorSetterTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        var attachResult = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));
        Assert.Equal("Core", attachResult!["clrFlavor"]!.GetValue<string>());

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        ulong objectAddress = ParseHex(playerAddress);

        // 1) Resolution reelle de l'adresse native via ClrMD -- Vigor est
        // deja JITte par le warmup unique dans TestRoot.BuildGraph
        // (ObjectGraph.cs).
        var resolved = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Vigor")));
        Assert.True(resolved!["success"]!.GetValue<bool>());
        Assert.Equal("set_Vigor", resolved["methodName"]!.GetValue<string>());
        Assert.Equal("Double", resolved["parameterType"]!.GetValue<string>());
        Assert.False(resolved["isStatic"]!.GetValue<bool>());
        string nativeCodeAddressHex = resolved["nativeCodeAddress"]!.GetValue<string>();
        ulong nativeCodeAddress = ParseHex(nativeCodeAddressHex);
        Assert.NotEqual(0UL, nativeCodeAddress);

        var statusBefore = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        int changeCountBefore = statusBefore!["player"]!["vigorChangeCount"]!.GetValue<int>();

        // 2) Appel reel du setter (via shellcode, XMM1) avec une valeur qui
        // DOIT etre clampee -- 250.0 > Player.VigorMax (100.0). Le bit
        // pattern IEEE754 du double est transmis tel quel (memcpy, pas de
        // conversion entiere), exactement comme
        // ApplicationController::encodeInstanceMethodParameterImmediate
        // cote natif.
        const double requestedValue = 250.0;
        ulong paramImmediate = unchecked((ulong)BitConverter.DoubleToInt64Bits(requestedValue));
        bool completed = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate, nativeCodeAddress, paramIsFloat: true);
        Assert.True(completed, "Le thread distant n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres l'appel shellcode du setter double (XMM1).");

        var statusAfter = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(100.0, statusAfter!["player"]!["vigor"]!.GetValue<double>(), 3);
        Assert.Equal(changeCountBefore + 1, statusAfter["player"]!["vigorChangeCount"]!.GetValue<int>());

        // Cross-verification independante : relecture ClrMD directe du champ
        // backing (_vigor), meme discipline que le test Vitality.
        var objAfter = await PipeClient.CallAsync(isolatedInspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        Assert.Equal(100.0, objAfter!["fields"]!["_vigor"]!.GetValue<double>(), 3);
    }

    [Fact]
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithReferenceParameter_PassesExistingObjectAddressAndProducesSideEffect()
    {
        // Chantier "setters a parametre objet/string" : Player.set_EquippedItem
        // prend un parametre de type REFERENCE (Item), pas primitif ni struct
        // -- RDX porte directement l'adresse d'un objet Item DEJA EXISTANT sur
        // le tas (pas de nouvelle allocation, hors scope arbitre en amont,
        // voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md). Preuve que le VRAI
        // setter tourne (pas une ecriture brute du champ backing
        // _equippedItem) : IsArmed change EN MEME TEMPS que la reference, et
        // EquipChangeCount s'incremente separement -- meme discipline de
        // preuve que Vitality/Vigor plus haut. Process isole pour ne pas
        // affecter le Player partage par les autres tests.
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_ReferenceSetterTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_ReferenceSetterTest";

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        var attachResult = await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));
        Assert.Equal("Core", attachResult!["clrFlavor"]!.GetValue<string>());

        var found = await PipeClient.CallAsync(
            isolatedInspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        ulong objectAddress = ParseHex(playerAddress);

        // Retrouve l'adresse REELLE de l'objet Item "Shield" deja existant sur
        // le tas (le warmup dans BuildGraph a equipe "Sword" -- ce test doit
        // pointer vers un AUTRE objet existant, pas re-equiper le meme, pour
        // prouver que la reference ecrite est bien celle demandee).
        var shieldLocator = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "findObjectsByFieldValue",
            new JsonArray(
                JsonValue.Create("KillEngine.ClrTestTarget.Item"), JsonValue.Create("Name"),
                JsonValue.Create("Shield"), JsonValue.Create(5)));
        Assert.True(shieldLocator!["success"]!.GetValue<bool>());
        string shieldAddress = Assert.Single(shieldLocator["matches"]!.AsArray())!["address"]!.GetValue<string>();
        ulong shieldObjectAddress = ParseHex(shieldAddress);

        // 1) Resolution reelle -- parametre non primitif resolu comme type
        // REFERENCE (pas struct) : parameterIsReferenceType doit etre true.
        var resolved = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "resolveInstanceMethodAddress",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("EquippedItem")));
        Assert.True(resolved!["success"]!.GetValue<bool>());
        Assert.Equal("set_EquippedItem", resolved["methodName"]!.GetValue<string>());
        Assert.Equal("KillEngine.ClrTestTarget.Item", resolved["parameterType"]!.GetValue<string>());
        Assert.True(resolved["parameterIsReferenceType"]!.GetValue<bool>());
        Assert.False(resolved["isStatic"]!.GetValue<bool>());
        string nativeCodeAddressHex = resolved["nativeCodeAddress"]!.GetValue<string>();
        ulong nativeCodeAddress = ParseHex(nativeCodeAddressHex);
        Assert.NotEqual(0UL, nativeCodeAddress);

        var statusBefore = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal("Sword", statusBefore!["player"]!["equippedItemName"]!.GetValue<string>());
        int equipChangeCountBefore = statusBefore["player"]!["equipChangeCount"]!.GetValue<int>();

        // 2) Appel reel du setter (shellcode) -- RDX porte DIRECTEMENT
        // l'adresse de l'objet Shield, pas de conversion IEEE754/entiere
        // comme pour les primitifs (plus simple a encoder que le cas
        // float/double).
        bool completed = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: shieldObjectAddress, nativeCodeAddress);
        Assert.True(completed, "Le thread distant n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres l'appel shellcode du setter reference.");

        var statusAfter = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal("Shield", statusAfter!["player"]!["equippedItemName"]!.GetValue<string>());
        Assert.True(statusAfter["player"]!["isArmed"]!.GetValue<bool>());
        Assert.Equal(equipChangeCountBefore + 1, statusAfter["player"]!["equipChangeCount"]!.GetValue<int>());

        // Cross-verification independante de l'oracle cote cible : relecture
        // ClrMD directe -- le champ reference backing (_equippedItem) doit
        // pointer vers l'adresse Shield.
        var objAfter = await PipeClient.CallAsync(isolatedInspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        var equippedItemField = Field(objAfter!["fields"]!, "_equippedItem");
        Assert.NotNull(equippedItemField);
        Assert.Equal(shieldAddress, equippedItemField!["address"]!.GetValue<string>(), StringComparer.OrdinalIgnoreCase);

        // 3) Deuxieme appel reel avec "null" -- efface la reference (0 en
        // RDX), meme convention que ClrSession.ParseReferenceValue deja
        // utilisee ailleurs dans ce module pour writePrimitivePath. Preuve
        // supplementaire que la logique du VRAI setter tourne : IsArmed doit
        // redevenir false, EquipChangeCount continue de s'incrementer.
        bool completedNull = NativeSetterInvoker.InvokeInstanceMethod(
            isolatedTarget.Pid, objectAddress, hasParam: true, paramImmediate: 0UL, nativeCodeAddress);
        Assert.True(completedNull, "Le thread distant (appel null) n'a pas termine dans le delai imparti.");
        Assert.False(isolatedTarget.Process.HasExited, "La cible a plante apres le deuxieme appel shellcode (null).");

        var statusAfterNull = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Null(statusAfterNull!["player"]!["equippedItemName"]);
        Assert.False(statusAfterNull["player"]!["isArmed"]!.GetValue<bool>());
        Assert.Equal(equipChangeCountBefore + 2, statusAfterNull["player"]!["equipChangeCount"]!.GetValue<int>());
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesPrimitiveArrayElementDirectly()
    {
        // PHASE 59 -- chantier "ecriture directe par index dans un tableau
        // primitif" : Player.Scores (int[]) doit pouvoir etre ecrit
        // directement par index, pas seulement lu -- distinct du cas deja
        // couvert (tableaux/List<T> de REFERENCES, ex: Inventory.Items[0]).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var write = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Scores[2]"), JsonValue.Create("777")));
        Assert.True(write!["verified"]!.GetValue<bool>());
        Assert.Equal(777, write["value"]!.GetValue<int>());

        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(777, status!["player"]!["scores"]![2]!.GetValue<int>());

        // Garde-fou : index hors limites doit rester rejete proprement (pas
        // d'ecriture hors tableau).
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePath",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Scores[99]"), JsonValue.Create("1")));
        });
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesStructArrayElementFieldDirectly()
    {
        // Chantier "ecriture indexee dans des tableaux de STRUCTS" :
        // Inventory.Waypoints (Coordinates[]) -- Waypoints[1].X doit pouvoir
        // etre ecrit directement. Composition de deux primitives DEJA
        // existantes separement (adresse d'element de tableau via
        // GetArrayElementAddress, adresse de champ dans un struct deja
        // localise via ClrInstanceField.GetAddress(interior:true)) --
        // ResolveIndexedReference/ResolveArrayElementNode (ClrSession.cs)
        // les enchaine desormais pour ce cas precis, la ou l'index seul
        // (element ENTIER, pas un de ses champs) reste rejete plus bas.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var write = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Waypoints[1].X"), JsonValue.Create("555")));
        Assert.True(write!["verified"]!.GetValue<bool>());
        Assert.Equal(555, write["value"]!.GetValue<int>());

        // Relecture ClrMD independante -- confirme que seul X a change, Y
        // (2, pose dans BuildGraph) est intact.
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var waypointsField = Field(inventoryObj!["fields"]!, "Waypoints");
        var waypointItems = waypointsField!["collection"]!["items"]!.AsArray();
        Assert.Equal(555, waypointItems[1]!["fields"]!["X"]!.GetValue<int>());
        Assert.Equal(2, waypointItems[1]!["fields"]!["Y"]!.GetValue<int>());

        // Oracle independant de ClrMD : pipe de controle de la cible.
        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(555, status!["player"]!["waypoint1X"]!.GetValue<int>());
        Assert.Equal(2, status["player"]!["waypoint1Y"]!.GetValue<int>());

        // L'ELEMENT ENTIER par index (Waypoints[1] seul, sans champ suivant)
        // est desormais supporte aussi (chantier "collections concurrentes
        // et ecriture d'element struct entier", format "Champ=Valeur,...") --
        // voir WritePrimitivePath_UpdatesWholeStructArrayElementByIndex.
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesWholeStructArrayElementByIndex()
    {
        // Dernier point laisse ouvert par le chantier "ecriture indexee dans
        // des tableaux de STRUCTS" (PHASE 59/66, voir le test precedent) :
        // ecrire Waypoints[1] ENTIER (les deux champs X et Y d'un coup), pas
        // seulement Waypoints[1].X isolement. Format retenu (docs/
        // KILLENGINE_CLR_INSPECTOR_SPEC.md, "format de saisie a definir") :
        // "X=777,Y=888" -- tous les champs du struct sont requis.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var write = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Waypoints[2]"), JsonValue.Create("X=777,Y=888")));
        Assert.True(write!["verified"]!.GetValue<bool>());

        // Relecture ClrMD independante -- confirme les DEUX champs ecrits.
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var waypointItems = Field(inventoryObj!["fields"]!, "Waypoints")!["collection"]!["items"]!.AsArray();
        Assert.Equal(777, waypointItems[2]!["fields"]!["X"]!.GetValue<int>());
        Assert.Equal(888, waypointItems[2]!["fields"]!["Y"]!.GetValue<int>());
        // Waypoints[0] intact -- confirme que seul l'element vise a change.
        Assert.Equal(1, waypointItems[0]!["fields"]!["X"]!.GetValue<int>());
        Assert.Equal(1, waypointItems[0]!["fields"]!["Y"]!.GetValue<int>());

        // Garde-fou 1 : champ manquant -- rejet propre, pas d'ecriture
        // partielle silencieuse (Y non fourni).
        var missingFieldEx = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePath",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Waypoints[0]"), JsonValue.Create("X=42")));
        });
        Assert.Contains("manquant", missingFieldEx.Message, StringComparison.OrdinalIgnoreCase);

        // Garde-fou 1 bis : Waypoints[0] bien INCHANGE apres le rejet
        // ci-dessus (pas de X=42 partiel malgre l'echec sur Y).
        var afterRejected = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var waypointsAfterRejected = Field(afterRejected!["fields"]!, "Waypoints")!["collection"]!["items"]!.AsArray();
        Assert.Equal(1, waypointsAfterRejected[0]!["fields"]!["X"]!.GetValue<int>());
        Assert.Equal(1, waypointsAfterRejected[0]!["fields"]!["Y"]!.GetValue<int>());

        // Garde-fou 2 : champ inconnu -- rejet propre.
        var unknownFieldEx = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePath",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Waypoints[0]"), JsonValue.Create("X=1,Y=2,Z=3")));
        });
        Assert.Contains("inconnu", unknownFieldEx.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesFieldNestedTwoStructLevelsInsideArrayElement()
    {
        // Chantier "struct-dans-struct-dans-tableau en ecriture" (docs/
        // POWER_UP_ROADMAP.md, "Extensions futures non bloquantes") : un
        // niveau de plus que WritePrimitivePath_UpdatesStructArrayElementFieldDirectly
        // ci-dessus (Waypoints[i].X, un seul niveau de struct sous l'element
        // de tableau). Ici Inventory.Zones (Zone[]) et Zone contient lui-meme
        // un struct Coordinates (Origin) -- Zones[0].Origin.X est donc a DEUX
        // niveaux de struct sous l'element de tableau (tableau -> Zone ->
        // Coordinates -> X). Le commentaire d'IsIndexedFieldPrimitiveArray/
        // IsIndexedFieldStructArray (ClrSession.cs) notait que la composition
        // generique PathNode/ResolvePathSegment devrait deja gerer ce cas
        // sans changement de code, juste jamais verifie explicitement -- ce
        // test le confirme (ou l'infirme) en conditions ClrMD reelles.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var write = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Zones[0].Origin.X"), JsonValue.Create("999")));
        Assert.True(write!["verified"]!.GetValue<bool>());
        Assert.Equal(999, write["value"]!.GetValue<int>());

        // Relecture ClrMD independante -- confirme que seul Origin.X a
        // change : Origin.Y et Radius (poses dans BuildGraph) restent
        // intacts, et Zones[1] n'est pas affecte.
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var zoneItems = Field(inventoryObj!["fields"]!, "Zones")!["collection"]!["items"]!.AsArray();
        var zone0Origin = Field(zoneItems[0]!["fields"]!, "Origin")!["fields"]!;
        Assert.Equal(999, zone0Origin["X"]!.GetValue<int>());
        Assert.Equal(200, zone0Origin["Y"]!.GetValue<int>());
        Assert.Equal(5, Field(zoneItems[0]!["fields"]!, "Radius")!.GetValue<int>());
        var zone1Origin = Field(zoneItems[1]!["fields"]!, "Origin")!["fields"]!;
        Assert.Equal(300, zone1Origin["X"]!.GetValue<int>());
        Assert.Equal(400, zone1Origin["Y"]!.GetValue<int>());

        // Oracle independant de ClrMD : pipe de controle de la cible.
        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(999, status!["player"]!["zone0OriginX"]!.GetValue<int>());
        Assert.Equal(200, status["player"]!["zone0OriginY"]!.GetValue<int>());
        Assert.Equal(5, status["player"]!["zone0Radius"]!.GetValue<int>());
    }

    [Fact]
    public async Task WritePrimitivePath_UpdatesWholeStructArrayElementWithNestedNonPrimitiveField()
    {
        // Chantier "struct-dans-tableau-de-structs en ecriture ENTIERE" : le
        // meme format "Champ=Valeur" que WritePrimitivePath_UpdatesWholeStructArrayElementByIndex
        // ci-dessus, mais sur Inventory.Zones (Zone[]) dont l'element (Zone)
        // contient lui-meme un struct imbrique (Origin: Coordinates) --
        // l'ancienne implementation rejetait ce cas ("champ non primitif").
        // Format retenu pour les champs imbriques : cles a plat en points
        // ("Origin.X=..,Origin.Y=..,Radius=..").
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        var write = await PipeClient.CallAsync(
            InspectorPipe,
            "writePrimitivePath",
            new JsonArray(
                JsonValue.Create(playerAddress),
                JsonValue.Create("Inventory.Zones[1]"),
                JsonValue.Create("Origin.X=111,Origin.Y=222,Radius=15")));
        Assert.True(write!["verified"]!.GetValue<bool>());

        // Relecture ClrMD independante -- confirme les TROIS champs feuilles
        // ecrits, et que Zones[0] (ecrit par le test precedent) est intact.
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var zoneItems = Field(inventoryObj!["fields"]!, "Zones")!["collection"]!["items"]!.AsArray();
        var zone1Origin = Field(zoneItems[1]!["fields"]!, "Origin")!["fields"]!;
        Assert.Equal(111, zone1Origin["X"]!.GetValue<int>());
        Assert.Equal(222, zone1Origin["Y"]!.GetValue<int>());
        Assert.Equal(15, Field(zoneItems[1]!["fields"]!, "Radius")!.GetValue<int>());
        // Zones[0] n'est pas la cible de CE test, mais partage le meme fixture
        // que WritePrimitivePath_UpdatesFieldNestedTwoStructLevelsInsideArrayElement
        // (qui ecrit Origin.X=999) -- xUnit ne garantit pas d'ordre d'execution
        // entre les deux (meme prudence que ReadObject_UnpacksNestedStructInsideStructRecursively
        // plus haut pour Stats.Rank) : seule la valeur ORIGINALE (200/5, jamais
        // ecrite par aucun test) est verifiee a une valeur fixe.
        var zone0Origin = Field(zoneItems[0]!["fields"]!, "Origin")!["fields"]!;
        Assert.True(zone0Origin["X"]!.GetValue<int>() is 100 or 999);
        Assert.Equal(200, zone0Origin["Y"]!.GetValue<int>());
        Assert.Equal(5, Field(zoneItems[0]!["fields"]!, "Radius")!.GetValue<int>());

        // Garde-fou 1 : champ feuille manquant -- rejet propre.
        var missingFieldEx = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePath",
                new JsonArray(JsonValue.Create(playerAddress), JsonValue.Create("Inventory.Zones[0]"), JsonValue.Create("Origin.X=1")));
        });
        Assert.Contains("manquant", missingFieldEx.Message, StringComparison.OrdinalIgnoreCase);

        // Garde-fou 2 : champ feuille inconnu -- rejet propre.
        var unknownFieldEx = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePath",
                new JsonArray(
                    JsonValue.Create(playerAddress),
                    JsonValue.Create("Inventory.Zones[0]"),
                    JsonValue.Create("Origin.X=1,Origin.Y=2,Radius=3,Origin.Z=4")));
        });
        Assert.Contains("inconnu", unknownFieldEx.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task PathWriteViaLocator_RefindsObjectAfterCompactingGcAndWritesNewAddress()
    {
        // PHASE 59 -- chantier "mutation par chemin symbolique auto-
        // relocalise apres GC" : reproduit exactement la composition faite
        // cote natif par ApplicationController::writeClrPrimitivePathByLocator
        // (relocaliser via findObjectsByFieldValue PUIS deleguer a
        // writePrimitivePath, sans jamais reutiliser une adresse memorisee).
        // ApplicationController est un objet Qt/C++ inaccessible depuis xUnit
        // sans harness d'automation Qt (meme limite deja documentee pour le
        // mecanisme shellcode, voir ResolveInstanceMethodAddress_
        // ThenRealShellcodeCall_...) -- ce test prouve donc le MECANISME
        // sous-jacent : locator + ecriture retrouvent et mutent correctement
        // l'objet apres un GC compactant reel, sans jamais fournir d'adresse
        // figee entre les deux resolutions.
        const string isolatedTargetPipe = "KillEngineClrTestTargetPipe_LocatorWriteTest";
        const string isolatedInspectorPipe = "KillEngineClrInspectorPipe_LocatorWriteTest";
        string targetDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tests", "clr_targets", "KillEngineClrTestTarget"), "KillEngineClrTestTarget.dll");
        string inspectorDll = BuiltAssemblyLocator.FindDll(
            Path.Combine("tools", "clr_inspector", "KillEngineClrInspector"), "KillEngineClrInspector.dll");

        await using var isolatedTarget = await ManagedProcessFixture.StartAsync(
            targetDll, isolatedTargetPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_TEST_TARGET_PIPE_NAME"] = isolatedTargetPipe });
        await using var isolatedInspector = await ManagedProcessFixture.StartAsync(
            inspectorDll, isolatedInspectorPipe, TimeSpan.FromSeconds(20),
            new Dictionary<string, string> { ["KILLENGINE_CLR_INSPECTOR_PIPE_NAME"] = isolatedInspectorPipe });

        await PipeClient.CallAsync(isolatedInspectorPipe, "attach", new JsonArray(JsonValue.Create(isolatedTarget.Pid)));

        async Task<string> ResolveAddressByLocatorAsync()
        {
            var located = await PipeClient.CallAsync(
                isolatedInspectorPipe,
                "findObjectsByFieldValue",
                new JsonArray(
                    JsonValue.Create("KillEngine.ClrTestTarget.Player"),
                    JsonValue.Create("Name"),
                    JsonValue.Create("TestSubject"),
                    JsonValue.Create(5)));
            Assert.True(located!["success"]!.GetValue<bool>());
            return Assert.Single(located["matches"]!.AsArray())!["address"]!.GetValue<string>();
        }

        string addressBefore = await ResolveAddressByLocatorAsync();
        Assert.False(string.IsNullOrWhiteSpace(addressBefore));

        // GC compactant reel avec churn -- deplace potentiellement le Player
        // (pas garanti a 100% pour CET objet precis, meme reserve deja
        // documentee dans FullMvpWorkflow_FindsSameLogicalObjectAfterCompactingGc :
        // la preuve centrale est que le mecanisme de re-resolution
        // fonctionne dans les deux cas, pas une inegalite d'adresse forcee).
        await PipeClient.CallAsync(isolatedTargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(50_000)));
        await PipeClient.CallAsync(isolatedTargetPipe, "forceGC");
        await PipeClient.CallAsync(isolatedTargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));
        await PipeClient.CallAsync(isolatedInspectorPipe, "flushCachedData");

        // Rejoue la MEME operation via locator, sans jamais donner l'adresse
        // -- doit retrouver le nouvel objet et ecrire correctement dessus.
        string addressAfter = await ResolveAddressByLocatorAsync();
        var write = await PipeClient.CallAsync(
            isolatedInspectorPipe,
            "writePrimitivePath",
            new JsonArray(JsonValue.Create(addressAfter), JsonValue.Create("Health"), JsonValue.Create("31415")));
        Assert.True(write!["verified"]!.GetValue<bool>());

        var status = await PipeClient.CallAsync(isolatedTargetPipe, "getStatus");
        Assert.Equal(31415, status!["player"]!["health"]!.GetValue<int>());
    }

    [Fact]
    public async Task WritePrimitivePathBatch_AppliesAllOperationsCorrectlyUnderConcurrentGcChurnPressure()
    {
        // PHASE 59 -- chantier "transaction atomique avec suspension
        // coordonnee du runtime" : ApplicationController::
        // writeClrPrimitivePathBatchAtomic (cote natif) enveloppe CET appel
        // RPC existant (writePrimitivePathBatch) dans un
        // killcore::ProcessThreadsSuspendGuard -- rien de nouveau cote
        // helper .NET, la garantie de suspension est entierement apportee
        // par ApplicationController. Aucun harness Qt/C++ n'existe dans ce
        // depot pour piloter ApplicationController depuis xUnit (meme limite
        // deja documentee ailleurs dans ce fichier) -- ce test NE PROUVE
        // DONC PAS la suspension elle-meme (aucune assertion "aucune autre
        // thread n'a tourne pendant la fenetre" n'est faite, ce serait une
        // fausse preuve). Il verifie uniquement que la transaction
        // multi-champs sous-jacente reste FONCTIONNELLEMENT correcte (toutes
        // les valeurs ecrites et verifiees) sous une pression memoire
        // concurrente reelle (GcChurnWorker a taux eleve pendant l'appel).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();

        await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(80_000)));
        try
        {
            var batch = await PipeClient.CallAsync(
                InspectorPipe,
                "writePrimitivePathBatch",
                new JsonArray(
                    JsonValue.Create(playerAddress),
                    new JsonArray(
                        new JsonObject { ["path"] = "Health", ["value"] = "5150" },
                        new JsonObject { ["path"] = "Scores[0]", ["value"] = "111" },
                        new JsonObject { ["path"] = "Stats.Rank", ["value"] = "9" })));

            Assert.True(batch!["success"]!.GetValue<bool>());
            Assert.Equal(3, batch["applied"]!.GetValue<int>());
        }
        finally
        {
            await PipeClient.CallAsync(TargetPipe, "setChurnRate", new JsonArray(JsonValue.Create(10)));
        }

        var status = await PipeClient.CallAsync(TargetPipe, "getStatus");
        Assert.Equal(5150, status!["player"]!["health"]!.GetValue<int>());
        Assert.Equal(111, status["player"]!["scores"]![0]!.GetValue<int>());
        Assert.Equal(9, status["player"]!["statsRank"]!.GetValue<int>());
    }

    [Fact]
    public async Task ReadObject_UnpacksHashSetQueueStackAndMultiDimArrayCorrectly()
    {
        // Chantier "Plus de collections BCL dans le deballage" : HashSet<T>,
        // Queue<T>, Stack<T> et tableau multidimensionnel (Inventory.Tags/
        // ItemQueue/ItemStack/Grid, ObjectGraph.cs). Le graphe force
        // deliberement un vrai wraparound de buffer circulaire (Queue) et une
        // vraie entree free-list (HashSet, apres un Remove()) -- voir les
        // commentaires de ClrSession.DescribeHashSet/DescribeQueue pour le
        // detail de ce qui a ete verifie par attache reelle avant d'ecrire ce
        // code (pas devine).
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var inventoryFields = inventoryObj!["fields"]!;

        // HashSet<string> Tags : "common"/"starter"/"verified" vivants,
        // "temp" ajoute PUIS retire (entree free-list qui ne doit PAS
        // apparaitre). count doit refleter le compte REEL (3), pas le nombre
        // de slots physiquement alloues.
        var tags = Field(inventoryFields, "Tags")!["collection"]!;
        Assert.Equal("hashset", tags["kind"]!.GetValue<string>());
        Assert.Equal(3, tags["count"]!.GetValue<int>());
        var tagValues = tags["items"]!.AsArray().Select(v => v!.GetValue<string>()).OrderBy(v => v).ToArray();
        Assert.Equal(new[] { "common", "starter", "verified" }, tagValues);

        // Queue<Item> ItemQueue : Enqueue(sword,shield,potion) puis
        // Dequeue() puis Enqueue(sword) force un vrai wraparound circulaire
        // (_head=1, _tail revient a 0) -- ordre logique attendu (avant vers
        // arriere) : Shield, Potion, Sword.
        var queue = Field(inventoryFields, "ItemQueue")!["collection"]!;
        Assert.Equal("queue", queue["kind"]!.GetValue<string>());
        Assert.Equal(3, queue["count"]!.GetValue<int>());
        var queueNames = new List<string>();
        foreach (var item in queue["items"]!.AsArray())
        {
            var itemObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(item!["address"]!.GetValue<string>())));
            queueNames.Add(itemObj!["fields"]!["Name"]!.GetValue<string>());
        }
        Assert.Equal(new[] { "Shield", "Potion", "Sword" }, queueNames);

        // Stack<Item> ItemStack : Push(potion), Push(sword), Pop(), Push(shield)
        // -- contenu final bas->haut = [potion, shield]. Restitue "sommet
        // d'abord" (Pop()) : [Shield, Potion].
        var stack = Field(inventoryFields, "ItemStack")!["collection"]!;
        Assert.Equal("stack", stack["kind"]!.GetValue<string>());
        Assert.Equal(2, stack["count"]!.GetValue<int>());
        var stackNames = new List<string>();
        foreach (var item in stack["items"]!.AsArray())
        {
            var itemObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(item!["address"]!.GetValue<string>())));
            stackNames.Add(itemObj!["fields"]!["Name"]!.GetValue<string>());
        }
        Assert.Equal(new[] { "Shield", "Potion" }, stackNames);

        // int[,] Grid (3x4), rempli par row*10+col -- deballage en liste
        // plate "row-major" bornee, avec les dimensions exposees separement.
        var grid = Field(inventoryFields, "Grid")!["collection"]!;
        Assert.Equal("multidim_array", grid["kind"]!.GetValue<string>());
        Assert.Equal(2, grid["rank"]!.GetValue<int>());
        Assert.Equal(new[] { 3, 4 }, grid["dimensions"]!.AsArray().Select(v => v!.GetValue<int>()));
        Assert.Equal(
            new[] { 0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23 },
            grid["items"]!.AsArray().Select(v => v!.GetValue<int>()));
    }

    [Fact]
    public async Task ReadObject_UnpacksLinkedListSortedDictionaryAndSortedSetInLogicalOrder()
    {
        // Chantier "LinkedList<T>/SortedDictionary<K,V>/SortedSet<T> dans le
        // deballage" -- layout interne verifie par attache ClrMD reelle avant
        // d'ecrire ClrSession.DescribeLinkedList/DescribeSortedDictionary/
        // DescribeSortedSet (script jetable, pas devine, voir les
        // commentaires de ces methodes pour le detail). Le graphe de test
        // (Inventory.LinkedTags/SortedCurrencies/SortedScores, ObjectGraph.cs)
        // peuple ces 3 collections dans un ORDRE D'ALLOCATION deliberement
        // different de l'ordre LOGIQUE attendu -- ce test verifie que
        // readObject restitue bien l'ordre LOGIQUE (liste chainee : ordre
        // d'insertion logique ; collections triees : ordre de tri), pas
        // l'ordre d'allocation memoire.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var inventoryFields = inventoryObj!["fields"]!;

        // LinkedList<string> LinkedTags : AddLast(second), AddLast(temp),
        // AddFirst(first), AddLast(third), Remove(temp) -- ordre logique
        // final attendu (tete -> queue) : first, second, third. La liste
        // est CIRCULAIRE en interne (verifie par attache reelle) : ce test
        // couvre implicitement que le deballage s'arrete correctement au
        // lieu de boucler indefiniment.
        var linkedTags = Field(inventoryFields, "LinkedTags")!["collection"]!;
        Assert.Equal("linkedlist", linkedTags["kind"]!.GetValue<string>());
        Assert.Equal(3, linkedTags["count"]!.GetValue<int>());
        Assert.Equal(
            new[] { "first", "second", "third" },
            linkedTags["items"]!.AsArray().Select(v => v!.GetValue<string>()));

        // SortedDictionary<string,int> SortedCurrencies : inserees dans
        // l'ordre silver/copper/gold -- ordre logique trie PAR CLE attendu :
        // copper, gold, silver.
        var sortedCurrencies = Field(inventoryFields, "SortedCurrencies")!["collection"]!;
        Assert.Equal("sorted_dictionary", sortedCurrencies["kind"]!.GetValue<string>());
        Assert.Equal(3, sortedCurrencies["count"]!.GetValue<int>());
        var currencyEntries = sortedCurrencies["entries"]!.AsArray();
        Assert.Equal(
            new[] { "copper", "gold", "silver" },
            currencyEntries.Select(e => e!["key"]!.GetValue<string>()));
        Assert.Equal(
            new[] { 9000, 12, 500 },
            currencyEntries.Select(e => e!["value"]!.GetValue<int>()));

        // SortedSet<int> SortedScores : inserees dans l'ordre 42/7/99/15 --
        // ordre logique trie attendu : 7, 15, 42, 99.
        var sortedScores = Field(inventoryFields, "SortedScores")!["collection"]!;
        Assert.Equal("sorted_set", sortedScores["kind"]!.GetValue<string>());
        Assert.Equal(4, sortedScores["count"]!.GetValue<int>());
        Assert.Equal(
            new[] { 7, 15, 42, 99 },
            sortedScores["items"]!.AsArray().Select(v => v!.GetValue<int>()));
    }

    [Fact]
    public async Task ReadObject_UnpacksNestedStructInsideStructRecursively()
    {
        // Chantier "Resolution recursive des structs imbriques" :
        // Player.Stats (PlayerStats) contient HomeZone (Zone), qui contient
        // Origin (Coordinates) -- 3 niveaux de struct avant d'atteindre les
        // feuilles primitives (X/Y/Radius). Avant ce chantier, un placeholder
        // texte fixe etait renvoye des le premier niveau de struct-dans-struct.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var found = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(found!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));

        var stats = playerObj!["fields"]!["Stats"]!;
        Assert.Equal("KillEngine.ClrTestTarget.PlayerStats", stats["typeName"]!.GetValue<string>());
        // Rank n'est PAS verifie a une valeur fixe : le meme fixture partage
        // (TargetAndInspectorFixture) est mute de facon permanente par
        // WritePrimitivePath_UpdatesStringReferenceStructAndDictionaryValues
        // (Stats.Rank = 42), et xUnit ne garantit pas d'ordre d'execution --
        // seule la STRUCTURE du deballage recursif nous interesse ici, pas la
        // valeur precise de ce champ particulier.
        Assert.True(stats["fields"]!["Rank"]!.GetValue<int>() is 7 or 42);

        var homeZone = stats["fields"]!["HomeZone"]!;
        Assert.Equal("KillEngine.ClrTestTarget.Zone", homeZone["typeName"]!.GetValue<string>());
        Assert.Equal(30, homeZone["fields"]!["Radius"]!.GetValue<int>());

        var origin = homeZone["fields"]!["Origin"]!;
        Assert.Equal("KillEngine.ClrTestTarget.Coordinates", origin["typeName"]!.GetValue<string>());
        Assert.Equal(12, origin["fields"]!["X"]!.GetValue<int>());
        Assert.Equal(-4, origin["fields"]!["Y"]!.GetValue<int>());
    }

    [Fact]
    public async Task FindStaticFields_ResolvesTestRootStaticFieldsDirectlyWithoutEnumerateRoots()
    {
        // Chantier "investigation du root StaticVar manquant" : heap.
        // EnumerateRoots() ne rapporte jamais TestRoot.RootPlayer comme root
        // StaticVar (limite documentee, confirmee ne pas etre resolue par
        // suspend:true ni ForceCompleteRuntimeEnumeration). findStaticFields
        // resout le meme besoin par un mecanisme different et fiable :
        // ClrType.StaticFields, qui fonctionne en attache PASSIVE.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var result = await PipeClient.CallAsync(
            InspectorPipe, "findStaticFields", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.TestRoot")));
        Assert.True(result!["success"]!.GetValue<bool>());
        var matches = result["matches"]!.AsArray();

        var rootPlayerMatch = Assert.Single(matches, m => m!["fieldName"]!.GetValue<string>() == "RootPlayer");
        Assert.Equal("KillEngine.ClrTestTarget.Player", rootPlayerMatch!["objectTypeName"]!.GetValue<string>());
        string objectAddress = rootPlayerMatch["objectAddress"]!.GetValue<string>();

        // Cross-verification : l'objet retrouve par le champ static est bien
        // lisible via readObject a cette meme adresse (mecanisme totalement
        // different de la resolution, meme resultat).
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(objectAddress)));
        Assert.Equal(objectAddress, playerObj!["address"]!.GetValue<string>());
        Assert.Equal("KillEngine.ClrTestTarget.Player", playerObj["typeName"]!.GetValue<string>());

        // DisposableSlot est un champ static reference nullable, actuellement
        // vide dans ce contexte partage (sauf s'il a ete peuple par un autre
        // test du meme fixture juste avant) -- on verifie seulement qu'il est
        // rapporte avec un objectAddress null OU une adresse plausible (0x...),
        // jamais une exception.
        var disposableSlotMatch = Assert.Single(matches, m => m!["fieldName"]!.GetValue<string>() == "DisposableSlot");
        Assert.True(disposableSlotMatch!["objectAddress"] is null || disposableSlotMatch["objectAddress"]!.GetValue<string>().StartsWith("0x"));
    }

    [Fact]
    public async Task FindGcRootPath_FindsPlausibleVerifiableChainFromRootToNestedItem()
    {
        // Chantier "GCRoot chain complet" -- le plus exploratoire du lot.
        // Cible : l'Item "Shield" (Inventory.Items[1]), atteignable UNIQUEMENT
        // via des references imbriquees (List<Item>._items, Queue<Item>._array
        // ou Stack<Item>._array -- PAS QuickSlots, qui ne contient que
        // sword/potion). Verifie honnetement que chaque saut du chemin
        // retourne correspond a une reference REELLEMENT lisible via
        // readObject sur l'objet precedent -- pas seulement "success:true".
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var itemsCollection = Field(inventoryObj!["fields"]!, "Items")!["collection"]!;
        string shieldAddress = itemsCollection["items"]![1]!["address"]!.GetValue<string>();
        var shieldObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(shieldAddress)));
        Assert.Equal("Shield", shieldObj!["fields"]!["Name"]!.GetValue<string>());

        var pathResult = await PipeClient.CallAsync(
            InspectorPipe, "findGcRootPath", new JsonArray(JsonValue.Create(shieldAddress), JsonValue.Create(8), JsonValue.Create(4000)));

        Assert.True(pathResult!["success"]!.GetValue<bool>(), pathResult["message"]?.GetValue<string>() ?? pathResult.ToString());
        var path = pathResult["path"]!.AsArray();
        Assert.NotEmpty(path);
        Assert.Equal(shieldAddress, path[^1]!["objectAddress"]!.GetValue<string>());
        // Chantier "vrai plus-court-chemin GCRoot" (BFS multi-source) :
        // FindGcRootPath garantit desormais le plus court chemin -- voir le
        // nouveau test dedie FindGcRootPath_ReturnsTheShorterOfTwoDistinctPaths
        // pour la preuve concrete (deux chemins de longueurs differentes vers
        // la meme cible). Ce test-ci continue de verifier le MECANISME
        // (chaque saut du chemin retourne est reellement lisible), inchange
        // par le passage au multi-source.
        Assert.True(pathResult["shortestPathGuaranteed"]!.GetValue<bool>());

        // Verifie chaque saut : l'objet COURANT (en partant de l'objet du
        // root) doit reellement exposer, via readObject, une reference vers
        // l'objet suivant du chemin -- preuve que le chemin n'est pas
        // fabrique, chaque maillon existe vraiment dans le graphe managé live.
        string currentAddress = pathResult["rootObjectAddress"]!.GetValue<string>();
        foreach (var step in path)
        {
            string expectedNext = step!["objectAddress"]!.GetValue<string>();
            var current = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(currentAddress)));
            Assert.True(
                JsonContainsAddress(current, expectedNext),
                $"Saut de chemin non verifiable : aucune reference vers {expectedNext} trouvee sur {currentAddress}.");
            currentAddress = expectedNext;
        }
    }

    [Fact]
    public async Task FindGcRootPath_ReturnsTheShorterOfTwoDistinctPaths()
    {
        // Chantier "vrai plus-court-chemin GCRoot" (BFS multi-source) --
        // preuve concrete que findGcRootPath retourne le PLUS COURT des deux
        // chemins existants, pas juste "un" chemin choisi par l'ordre
        // d'enumeration des roots. Le graphe de test (ObjectGraph.cs) expose
        // deliberement DEUX chemins de longueurs differentes vers la MEME
        // instance de ShortestPathProbe :
        //   - COURT (1 saut) : TestRoot.ShortestPathShortcutHandle (root
        //     StrongHandle distinct) -> ShortestPathShortcut.Target -> Probe.
        //   - LONG (4 sauts) : TestRoot.RootHandle -> Inventory ->
        //     LongChainStart -> Next -> Next -> Leaf -> Probe (le meme
        //     objet).
        // L'ancienne version (BFS independant par root, premier chemin
        // trouve gagne) aurait pu retourner l'un OU l'autre selon l'ordre
        // d'enumeration de heap.EnumerateRoots() -- pas garanti. Le BFS
        // multi-source doit TOUJOURS retourner le chemin a 1 saut, quel que
        // soit cet ordre.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundProbe = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.ShortestPathProbe")));
        string probeAddress = Assert.Single(foundProbe!.AsArray())!["address"]!.GetValue<string>();

        var pathResult = await PipeClient.CallAsync(
            InspectorPipe, "findGcRootPath", new JsonArray(JsonValue.Create(probeAddress), JsonValue.Create(8), JsonValue.Create(4000)));

        Assert.True(pathResult!["success"]!.GetValue<bool>(), pathResult["message"]?.GetValue<string>() ?? pathResult.ToString());
        Assert.True(pathResult["shortestPathGuaranteed"]!.GetValue<bool>());

        // Le chemin retourne doit etre le COURT (1 saut), pas le long (4
        // sauts) -- la garantie centrale de ce chantier.
        Assert.Equal(1, pathResult["depth"]!.GetValue<int>());
        var path = pathResult["path"]!.AsArray();
        Assert.Single(path);
        Assert.Equal(probeAddress, path[0]!["objectAddress"]!.GetValue<string>());

        // L'objet racine du chemin retenu doit etre le ShortestPathShortcut
        // (root du chemin COURT), pas l'Inventory (root du chemin LONG).
        Assert.Contains("ShortestPathShortcut", pathResult["rootObjectTypeName"]!.GetValue<string>());

        // Verification independante : le chemin long existe bel et bien
        // aussi dans le graphe reel (pas juste suppose par construction) --
        // le lit via readObject en partant d'Inventory pour confirmer que
        // les 4 sauts sont reellement presents, meme si ce n'est pas celui
        // que findGcRootPath a choisi de retourner.
        var foundInventory = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Inventory")));
        string inventoryAddress = Assert.Single(foundInventory!.AsArray())!["address"]!.GetValue<string>();
        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var longChainStart = Field(inventoryObj!["fields"]!, "LongChainStart")!;
        string node1Address = longChainStart["address"]!.GetValue<string>();
        var node1Obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(node1Address)));
        var node2 = Field(node1Obj!["fields"]!, "Next")!;
        var node2Obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(node2["address"]!.GetValue<string>())));
        var node3 = Field(node2Obj!["fields"]!, "Next")!;
        var node3Obj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(node3["address"]!.GetValue<string>())));
        var leaf = Field(node3Obj!["fields"]!, "Leaf")!;
        Assert.Equal(probeAddress, leaf["address"]!.GetValue<string>());
    }

    [Fact]
    public async Task GenerateObjectReport_WalksReachableGraphAndIncludesGcRootChain()
    {
        // Chantier "rapport d'objet" : depuis Inventory (atteignable en 1 saut
        // via le root StrongHandle -- meme objet deja utilise par
        // FindGcRootPath_FindsPlausibleVerifiableChainFromRootToNestedItem,
        // choisi ici pour la meme raison : chemin GCRoot deterministe, pas
        // suppose). Verifie que le rapport (1) decrit bien la racine, (2)
        // traverse au moins un champ reference connu (Items, List<Item>) et
        // le rapporte avec sa provenance (discoveredVia), et (3) inclut un
        // chemin GCRoot reellement trouve (pas seulement "success:true").
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var report = await PipeClient.CallAsync(
            InspectorPipe,
            "generateObjectReport",
            new JsonArray(JsonValue.Create(inventoryAddress), JsonValue.Create(2), JsonValue.Create(30), JsonValue.Create(true)));

        Assert.True(report!["success"]!.GetValue<bool>());
        Assert.Equal(inventoryAddress, report["rootAddress"]!.GetValue<string>());
        Assert.Contains("Inventory", report["rootTypeName"]!.GetValue<string>());

        var nodes = report["nodes"]!.AsArray();
        Assert.True(nodes.Count > 1, "Le rapport n'a traverse aucune reference -- devrait au moins atteindre Items/QuickSlots/CustomItems.");

        var rootNode = nodes[0]!;
        Assert.Equal(inventoryAddress, rootNode["address"]!.GetValue<string>());
        Assert.Equal(0, rootNode["depth"]!.GetValue<int>());
        Assert.Null(rootNode["discoveredVia"]);

        // "Items" est une auto-propriete -- le champ CLR reel sous-jacent est
        // le backing field genere par le compilateur (<Items>k__BackingField),
        // pas "Items" litteralement (meme convention que le helper Field()
        // ci-dessous, deja utilise par les autres tests de ce fichier).
        // Match exact requis : "CustomItems" contient aussi la sous-chaine
        // "Items", un Contains() imprecis matcherait les deux champs.
        var itemsNode = Assert.Single(nodes, n =>
            n!["discoveredVia"] is JsonObject via && via["fieldName"]?.GetValue<string>() == "<Items>k__BackingField");
        Assert.Contains("List", itemsNode!["node"]!["typeName"]!.GetValue<string>());
        Assert.Equal(1, itemsNode["depth"]!.GetValue<int>());

        var gcRootChain = report["gcRootChain"];
        Assert.NotNull(gcRootChain);
        Assert.True(gcRootChain!["success"]!.GetValue<bool>(), gcRootChain["message"]?.GetValue<string>() ?? gcRootChain.ToString());
        Assert.Equal(inventoryAddress, gcRootChain["targetAddress"]!.GetValue<string>());
    }

    [Fact]
    public async Task ReadObject_UnpacksConcurrentDictionaryIgnoringRemovedEntries()
    {
        // Chantier "collections concurrentes" (docs/POWER_UP_ROADMAP.md
        // candidat #8, extension listee "non couverte a ce jour") -- layout
        // interne verifie par attache ClrMD reelle avant d'ecrire
        // ClrSession.DescribeConcurrentDictionary (script jetable, pas
        // devine, voir le commentaire de cette methode pour le detail :
        // _tables._buckets est un VolatileNode[] dont chaque struct porte
        // une reference _node vers une chaine de Node simplement liee, et le
        // compte reel est la somme de _tables._countPerLock, pas la longueur
        // de _buckets). Le graphe de test (Inventory.ConcurrentCounters,
        // ObjectGraph.cs) ajoute "hits"/"stale"/"misses", met a jour "hits"
        // via TryUpdate et retire "stale" via TryRemove -- ce test verifie
        // que le deballage ne rapporte que les 2 entrees reellement vivantes
        // avec leur valeur a jour, pas un noeud fantome laisse par le retrait.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var concurrentCounters = Field(inventoryObj!["fields"]!, "ConcurrentCounters")!["collection"]!;

        Assert.Equal("concurrent_dictionary", concurrentCounters["kind"]!.GetValue<string>());
        Assert.Equal(2, concurrentCounters["count"]!.GetValue<int>());
        Assert.Equal(2, concurrentCounters["returned"]!.GetValue<int>());
        Assert.False(concurrentCounters["truncated"]!.GetValue<bool>());

        var entries = concurrentCounters["entries"]!.AsArray()
            .ToDictionary(e => e!["key"]!.GetValue<string>(), e => e!["value"]!.GetValue<int>());
        Assert.Equal(new Dictionary<string, int> { ["hits"] = 41, ["misses"] = 7 }, entries);
        Assert.DoesNotContain("stale", entries.Keys);
    }

    [Fact]
    public async Task ReadObject_UnpacksConcurrentStackInLifoOrder()
    {
        // Extension non bloquante du chantier "collections concurrentes" :
        // ConcurrentStack<T> a un layout nettement plus simple que
        // ConcurrentQueue<T>/ConcurrentBag<T> (_head -> Node._next), donc on
        // le couvre sans ouvrir le chantier plus risqué des structures
        // segmentées/work-stealing.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var concurrentTags = Field(inventoryObj!["fields"]!, "ConcurrentTags")!["collection"]!;

        Assert.Equal("concurrent_stack", concurrentTags["kind"]!.GetValue<string>());
        Assert.Equal(3, concurrentTags["count"]!.GetValue<int>());
        Assert.Equal(3, concurrentTags["returned"]!.GetValue<int>());
        Assert.False(concurrentTags["truncated"]!.GetValue<bool>());

        var items = concurrentTags["items"]!.AsArray().Select(i => i!.GetValue<string>()).ToArray();
        Assert.Equal(new[] { "top", "middle", "bottom" }, items);
        Assert.DoesNotContain("temp-to-pop", items);
    }

    [Fact]
    public async Task ReadObject_UnpacksConcurrentQueueAcrossSegmentBoundaryInFifoOrder()
    {
        // Extension "collections concurrentes" laissee ouverte par le test
        // ConcurrentStack ci-dessus ("chantier plus risque des structures
        // segmentees") -- layout de ConcurrentQueue<T> verifie par reflection
        // sur le runtime .NET local (script jetable) PUIS valide contre
        // ToArray() sur 8 scenarios (vide, sequentiel, dequeue partiel,
        // multi-segment, wrap-around, drain+refill, frontiere de segment)
        // avant d'ecrire ClrSession.DescribeConcurrentQueue -- voir le
        // commentaire de cette methode pour le detail de l'algorithme MPMC
        // borne (Slot.SequenceNumber == pos + 1). Le graphe de test
        // (Inventory.ConcurrentEvents, ObjectGraph.cs) enqueue 40 elements
        // (capacite initiale de segment = 32, donc un deuxieme segment est
        // force) puis en dequeue 35 -- exerce deliberement la traversee de
        // frontiere de segment, pas seulement le cas a un seul segment.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var concurrentEvents = Field(inventoryObj!["fields"]!, "ConcurrentEvents")!["collection"]!;

        Assert.Equal("concurrent_queue", concurrentEvents["kind"]!.GetValue<string>());
        Assert.Equal(5, concurrentEvents["count"]!.GetValue<int>());
        Assert.Equal(5, concurrentEvents["returned"]!.GetValue<int>());
        Assert.False(concurrentEvents["truncated"]!.GetValue<bool>());

        var items = concurrentEvents["items"]!.AsArray().Select(i => i!.GetValue<string>()).ToArray();
        Assert.Equal(new[] { "evt-35", "evt-36", "evt-37", "evt-38", "evt-39" }, items);
    }

    [Fact]
    public async Task ReadObject_UnpacksConcurrentBagAfterCrossThreadStealFromDeadOwner()
    {
        // Extension "collections concurrentes" -- ConcurrentBag<T> est la
        // structure work-stealing a affinite de thread explicitement laissee
        // ouverte par PHASE 222 (voir le commentaire du test ConcurrentQueue
        // ci-dessus, "perimetre plus incertain"). Layout verifie par
        // reflection sur le runtime .NET local (script jetable) PUIS valide
        // empiriquement contre ToArray() sur 10 scenarios (vide, ajouts
        // simples, churn local sans croissance, croissance, drain+reajout,
        // plusieurs threads, vol cross-thread apres mort du proprietaire, vol
        // ET croissance combines, file videe a cote d'une vivante) avant
        // d'ecrire ClrSession.DescribeConcurrentBag -- voir son commentaire
        // pour le detail de l'algorithme (_headIndex/_tailIndex sont des
        // indices PLATS, pas de wraparound modulo malgre le champ _mask;
        // _headIndex n'avance que par vol, _tailIndex par Add/Take locaux;
        // enumeration en ordre inverse tailIndex-1 vers headIndex). Le graphe
        // de test (Inventory.ConcurrentTraces, ObjectGraph.cs) fait ajouter 6
        // elements par un thread QUI MEURT ENSUITE, puis le thread principal
        // vole 2 elements via TryTake (pas de file locale dans ce bag pour ce
        // thread) -- exerce deliberement _headIndex > 0 sur une file dont le
        // proprietaire n'existe plus.
        await PipeClient.CallAsync(InspectorPipe, "attach", new JsonArray(JsonValue.Create(_fixture.Target.Pid)));

        var foundPlayer = await PipeClient.CallAsync(
            InspectorPipe, "findObjectsByType", new JsonArray(JsonValue.Create("KillEngine.ClrTestTarget.Player")));
        string playerAddress = Assert.Single(foundPlayer!.AsArray())!["address"]!.GetValue<string>();
        var playerObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(playerAddress)));
        string inventoryAddress = Field(playerObj!["fields"]!, "Inventory")!["address"]!.GetValue<string>();

        var inventoryObj = await PipeClient.CallAsync(InspectorPipe, "readObject", new JsonArray(JsonValue.Create(inventoryAddress)));
        var concurrentTraces = Field(inventoryObj!["fields"]!, "ConcurrentTraces")!["collection"]!;

        Assert.Equal("concurrent_bag", concurrentTraces["kind"]!.GetValue<string>());
        Assert.Equal(4, concurrentTraces["count"]!.GetValue<int>());
        Assert.Equal(4, concurrentTraces["returned"]!.GetValue<int>());
        Assert.False(concurrentTraces["truncated"]!.GetValue<bool>());

        var items = concurrentTraces["items"]!.AsArray().Select(i => i!.GetValue<string>()).ToArray();
        Assert.Equal(new[] { "trace-5", "trace-4", "trace-3", "trace-2" }, items);
    }

    private static bool JsonContainsAddress(JsonNode? node, string address)
    {
        switch (node)
        {
            case JsonObject obj:
                if (obj.TryGetPropertyValue("address", out JsonNode? addressNode)
                    && addressNode is not null
                    && string.Equals(addressNode.GetValue<string>(), address, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
                foreach (var property in obj)
                {
                    if (JsonContainsAddress(property.Value, address)) return true;
                }
                return false;
            case JsonArray array:
                foreach (var item in array)
                {
                    if (JsonContainsAddress(item, address)) return true;
                }
                return false;
            default:
                return false;
        }
    }

    private static ulong ParseHex(string hex)
    {
        string trimmed = hex.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? hex[2..] : hex;
        return ulong.Parse(trimmed, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture);
    }

    private static JsonNode? Field(JsonNode fields, string publicName)
    {
        var obj = fields.AsObject();
        if (obj.TryGetPropertyValue(publicName, out JsonNode? direct))
        {
            return direct;
        }
        string backingName = $"<{publicName}>k__BackingField";
        return obj.TryGetPropertyValue(backingName, out JsonNode? backing) ? backing : null;
    }
}

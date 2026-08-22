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
    public async Task ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithDoubleParameter_LoadsXmm1AndProducesSideEffect()
    {
        // PHASE 58 -- chantier "setters float/double" : Player.set_Vigor
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
    public async Task WritePrimitivePath_UpdatesPrimitiveArrayElementDirectly()
    {
        // PHASE 58 -- chantier "ecriture directe par index dans un tableau
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
    public async Task PathWriteViaLocator_RefindsObjectAfterCompactingGcAndWritesNewAddress()
    {
        // PHASE 58 -- chantier "mutation par chemin symbolique auto-
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
        // PHASE 58 -- chantier "transaction atomique avec suspension
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

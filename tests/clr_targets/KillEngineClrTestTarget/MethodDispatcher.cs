using System.Text.Json.Nodes;

namespace KillEngine.ClrTestTarget;

/// <summary>
/// Surface de methodes pilotables via ControlPipeServer. Volontairement une
/// petite liste fixe (pas de reflexion generique comme AutomationPipeServer
/// cote C++) : cette cible n'a besoin d'exposer que ce que le futur
/// harness d'auto-test ClrMD (candidat #8) doit verifier, voir
/// docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md section "Points a valider".
/// </summary>
public sealed class MethodDispatcher
{
    private readonly GcChurnWorker _churn;
    private readonly Action _requestShutdown;
    private readonly DateTime _startedAtUtc = DateTime.UtcNow;

    public MethodDispatcher(GcChurnWorker churn, Action requestShutdown)
    {
        _churn = churn;
        _requestShutdown = requestShutdown;
    }

    public object? Invoke(string method, JsonArray args)
    {
        return method switch
        {
            "ping" => "pong: KillEngineClrTestTarget",
            "getStatus" => GetStatus(),
            "forceGC" => ForceGc(),
            "mutateField" => MutateField(args),
            "getObjectIdentity" => GetObjectIdentity(),
            "setChurnRate" => SetChurnRate(args),
            "spawnDisposable" => SpawnDisposable(args),
            "dropDisposable" => DropDisposable(),
            "shutdown" => Shutdown(),
            _ => throw new MethodDispatchException($"Methode inconnue : {method}"),
        };
    }

    private object GetStatus()
    {
        var player = TestRoot.RootPlayer;
        return new
        {
            pid = Environment.ProcessId,
            uptimeMs = (long)(DateTime.UtcNow - _startedAtUtc).TotalMilliseconds,
            gcGen0Count = GC.CollectionCount(0),
            gcGen1Count = GC.CollectionCount(1),
            gcGen2Count = GC.CollectionCount(2),
            totalMemoryBytes = GC.GetTotalMemory(false),
            churnObjectsPerSecond = _churn.ObjectsPerSecond,
            churnTotalAllocated = _churn.TotalAllocated,
            player = new
            {
                name = player.Name,
                health = player.Health,
                experience = player.Experience,
                stamina = player.Stamina,
                isAlive = player.IsAlive,
                vitality = player.Vitality,
                vitalityChangeCount = player.VitalityChangeCount,
                statsRank = player.Stats.Rank,
                statsLuck = player.Stats.Luck,
                itemCount = player.Inventory.Items.Count,
                firstItemValue = player.Inventory.Items.Count > 0 ? player.Inventory.Items[0].Value : (int?)null,
                quickSlot0Name = player.Inventory.QuickSlots[0]?.Name,
                quickSlot1Name = player.Inventory.QuickSlots[1]?.Name,
                quickSlot2Name = player.Inventory.QuickSlots[2]?.Name,
                gold = player.Inventory.Currencies.GetValueOrDefault("gold"),
                gems = player.Inventory.Currencies.GetValueOrDefault("gems"),
            },
        };
    }

    private object ForceGc()
    {
        int gen0Before = GC.CollectionCount(0);
        int gen1Before = GC.CollectionCount(1);
        int gen2Before = GC.CollectionCount(2);

        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        GC.WaitForPendingFinalizers();
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);

        return new
        {
            gen0Before,
            gen1Before,
            gen2Before,
            gen0After = GC.CollectionCount(0),
            gen1After = GC.CollectionCount(1),
            gen2After = GC.CollectionCount(2),
        };
    }

    private object MutateField(JsonArray args)
    {
        if (args.Count != 2)
        {
            throw new MethodDispatchException("mutateField attend [fieldPath, value].");
        }

        string fieldPath = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("fieldPath manquant.");
        JsonNode value = args[1] ?? throw new MethodDispatchException("value manquante.");
        var player = TestRoot.RootPlayer;

        switch (fieldPath)
        {
            case "player.health":
                player.Health = value.GetValue<int>();
                break;
            case "player.experience":
                player.Experience = value.GetValue<long>();
                break;
            case "player.stamina":
                player.Stamina = value.GetValue<float>();
                break;
            case "player.isAlive":
                player.IsAlive = value.GetValue<bool>();
                break;
            case "player.name":
                player.Name = value.GetValue<string>();
                break;
            case "inventory.currencies.gold":
                player.Inventory.Currencies["gold"] = value.GetValue<int>();
                break;
            case "inventory.items[0].value":
                if (player.Inventory.Items.Count == 0)
                {
                    throw new MethodDispatchException("inventory.Items est vide.");
                }
                player.Inventory.Items[0].Value = value.GetValue<int>();
                break;
            default:
                throw new MethodDispatchException(
                    $"Champ inconnu : {fieldPath}. Champs valides : player.health, player.experience, " +
                    "player.stamina, player.isAlive, player.name, inventory.currencies.gold, inventory.items[0].value.");
        }

        return new { fieldPath, applied = true };
    }

    private object GetObjectIdentity()
    {
        var player = TestRoot.RootPlayer;
        return new
        {
            player = TestRoot.StableIdentityOf(player),
            inventory = TestRoot.StableIdentityOf(player.Inventory),
            firstItem = player.Inventory.Items.Count > 0 ? TestRoot.StableIdentityOf(player.Inventory.Items[0]) : (int?)null,
            self = player.Self is not null ? TestRoot.StableIdentityOf(player.Self) : (int?)null,
            selfIsPlayer = ReferenceEquals(player, player.Self),
        };
    }

    private object SetChurnRate(JsonArray args)
    {
        if (args.Count != 1)
        {
            throw new MethodDispatchException("setChurnRate attend [objectsPerSecond].");
        }
        int rate = args[0]?.GetValue<int>() ?? throw new MethodDispatchException("objectsPerSecond manquant.");
        _churn.ObjectsPerSecond = rate;
        return new { churnObjectsPerSecond = _churn.ObjectsPerSecond };
    }

    private object SpawnDisposable(JsonArray args)
    {
        string tag = args.Count >= 1 && args[0] is not null ? args[0]!.GetValue<string>() : "regression-test";
        var probe = TestRoot.SpawnDisposable(tag);
        return new { id = probe.Id, tag = probe.Tag, identity = TestRoot.StableIdentityOf(probe) };
    }

    private object DropDisposable()
    {
        TestRoot.DropDisposable();
        return new { dropped = true };
    }

    private object Shutdown()
    {
        _requestShutdown();
        return "shutting down";
    }
}

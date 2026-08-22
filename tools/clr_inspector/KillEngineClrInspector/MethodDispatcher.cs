using System.Text.Json.Nodes;

namespace KillEngine.ClrInspector;

/// <summary>
/// Surface de methodes pilotables via ControlPipeServer -- l'API "propre"
/// que KillEngine.exe (C++) pourra appeler plus tard, exactement comme il
/// pilote deja apps/desktop/automation_pipe_server.h et
/// tests/clr_targets/KillEngineClrTestTarget. Volontairement minimal (MVP) :
/// couvre uniquement les 7 points demandes par l'utilisateur le 20/08/2026,
/// pas de panneau UI ni de fonctions avancees a ce stade.
/// </summary>
public sealed class MethodDispatcher
{
    private readonly ClrSession _session;
    private readonly Action _requestShutdown;

    public MethodDispatcher(ClrSession session, Action requestShutdown)
    {
        _session = session;
        _requestShutdown = requestShutdown;
    }

    public object? Invoke(string method, JsonArray args)
    {
        return method switch
        {
            "ping" => "pong: KillEngineClrInspector",
            "attach" => Attach(args),
            "detach" => _session.Detach(),
            "flushCachedData" => _session.FlushCachedData(),
            "findObjectsByType" => FindObjectsByType(args),
            "findObjectsByFieldValue" => FindObjectsByFieldValue(args),
            "readObject" => ReadObject(args),
            "writePrimitiveField" => WritePrimitiveField(args),
            "writePrimitivePath" => WritePrimitivePath(args),
            "writePrimitivePathBatch" => WritePrimitivePathBatch(args),
            "enumerateRoots" => EnumerateRoots(args),
            "shutdown" => Shutdown(),
            _ => throw new MethodDispatchException($"Methode inconnue : {method}"),
        };
    }

    private object Attach(JsonArray args)
    {
        if (args.Count != 1)
        {
            throw new MethodDispatchException("attach attend [pid].");
        }
        int pid = args[0]?.GetValue<int>() ?? throw new MethodDispatchException("pid manquant.");
        try
        {
            return _session.Attach(pid);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object FindObjectsByType(JsonArray args)
    {
        string typeSubstring = args.Count >= 1 && args[0] is not null
            ? args[0]!.GetValue<string>()
            : "KillEngine.ClrTestTarget";
        try
        {
            return _session.FindObjectsByTypeSubstring(typeSubstring);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object ReadObject(JsonArray args)
    {
        if (args.Count != 1)
        {
            throw new MethodDispatchException("readObject attend [addressHex].");
        }
        string addressHex = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("addressHex manquant.");
        try
        {
            return _session.ReadObject(addressHex);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object FindObjectsByFieldValue(JsonArray args)
    {
        if (args.Count < 3 || args.Count > 4)
        {
            throw new MethodDispatchException("findObjectsByFieldValue attend [typeSubstring, fieldName, expectedValue, maxResults?].");
        }
        string typeSubstring = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("typeSubstring manquant.");
        string fieldName = args[1]?.GetValue<string>() ?? throw new MethodDispatchException("fieldName manquant.");
        string expectedValue = args[2]?.ToString() ?? throw new MethodDispatchException("expectedValue manquante.");
        int maxResults = args.Count >= 4 && args[3] is not null ? args[3]!.GetValue<int>() : 20;
        try
        {
            return _session.FindObjectsByFieldValue(typeSubstring, fieldName, expectedValue, maxResults);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object EnumerateRoots(JsonArray args)
    {
        string? typeSubstring = args.Count >= 1 && args[0] is not null ? args[0]!.GetValue<string>() : null;
        try
        {
            return _session.EnumerateRoots(typeSubstring);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object WritePrimitiveField(JsonArray args)
    {
        if (args.Count != 3)
        {
            throw new MethodDispatchException("writePrimitiveField attend [objectAddressHex, fieldName, value].");
        }
        string objectAddressHex = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("objectAddressHex manquant.");
        string fieldName = args[1]?.GetValue<string>() ?? throw new MethodDispatchException("fieldName manquant.");
        string valueText = args[2]?.ToString() ?? throw new MethodDispatchException("value manquante.");
        try
        {
            return _session.WritePrimitiveField(objectAddressHex, fieldName, valueText);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object WritePrimitivePath(JsonArray args)
    {
        if (args.Count != 3)
        {
            throw new MethodDispatchException("writePrimitivePath attend [objectAddressHex, path, value].");
        }
        string objectAddressHex = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("objectAddressHex manquant.");
        string path = args[1]?.GetValue<string>() ?? throw new MethodDispatchException("path manquant.");
        string valueText = args[2]?.ToString() ?? throw new MethodDispatchException("value manquante.");
        try
        {
            return _session.WritePrimitivePath(objectAddressHex, path, valueText);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object WritePrimitivePathBatch(JsonArray args)
    {
        if (args.Count != 2)
        {
            throw new MethodDispatchException("writePrimitivePathBatch attend [objectAddressHex, operations].");
        }
        string objectAddressHex = args[0]?.GetValue<string>() ?? throw new MethodDispatchException("objectAddressHex manquant.");
        JsonArray operationsJson = args[1]?.AsArray() ?? throw new MethodDispatchException("operations manquantes.");
        var operations = new List<PathWriteOperation>();
        foreach (JsonNode? node in operationsJson)
        {
            JsonObject obj = node?.AsObject() ?? throw new MethodDispatchException("Chaque operation doit etre un objet { path, value }.");
            string path = obj["path"]?.GetValue<string>() ?? throw new MethodDispatchException("operation.path manquant.");
            string value = obj["value"]?.ToString() ?? throw new MethodDispatchException("operation.value manquante.");
            operations.Add(new PathWriteOperation(path, value));
        }
        try
        {
            return _session.WritePrimitivePathBatch(objectAddressHex, operations);
        }
        catch (ClrSessionException ex)
        {
            throw new MethodDispatchException(ex.Message);
        }
    }

    private object Shutdown()
    {
        _requestShutdown();
        return "shutting down";
    }
}

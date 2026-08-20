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
            "readObject" => ReadObject(args),
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

    private object Shutdown()
    {
        _requestShutdown();
        return "shutting down";
    }
}

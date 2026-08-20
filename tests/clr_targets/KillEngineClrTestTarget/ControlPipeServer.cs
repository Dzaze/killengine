using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace KillEngine.ClrTestTarget;

/// <summary>
/// Serveur de controle JSON-RPC sur named pipe Windows, protocole identique
/// (une ligne = une requete/reponse) a apps/desktop/automation_pipe_server.h
/// cote KillEngine.exe : {"id":1,"method":"...","params":[...]}
///                     -> {"id":1,"result":...} ou {"id":1,"error":"..."}
/// Reutilise volontairement le meme format pour que
/// scripts/automation-pipe-call.ps1 pilote cette cible sans modification,
/// juste avec -PipeName KillEngineClrTestTargetPipe.
///
/// Nom de pipe parametrable (pas juste la constante DefaultPipeName) : les
/// auto-tests de regression ont besoin de lancer des instances
/// supplementaires isolees (kill abrupt, redemarrage nouveau PID) sans
/// collision avec l'instance partagee du fixture -- voir
/// KillEngineClrInspector.Tests/EndToEndTests.cs.
/// </summary>
public sealed class ControlPipeServer
{
    public const string DefaultPipeName = "KillEngineClrTestTargetPipe";

    public string PipeName { get; }

    private readonly MethodDispatcher _dispatcher;
    private volatile bool _stopRequested;

    public ControlPipeServer(MethodDispatcher dispatcher, string? pipeName = null)
    {
        _dispatcher = dispatcher;
        PipeName = pipeName ?? DefaultPipeName;
    }

    public void RequestStop() => _stopRequested = true;

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        Console.WriteLine($"ControlPipeServer: ecoute sur \\\\.\\pipe\\{PipeName}");

        while (!_stopRequested && !cancellationToken.IsCancellationRequested)
        {
            using var server = new NamedPipeServerStream(
                PipeName,
                PipeDirection.InOut,
                NamedPipeServerStream.MaxAllowedServerInstances,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            try
            {
                await server.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            // Une connexion a la fois est traitee ici avant d'en accepter une
            // nouvelle (meme usage que le client PowerShell existant : une
            // requete, une reponse, ferme). Suffisant pour une cible de test
            // pilotee par un harness, pas besoin du multi-connexion concurrent
            // du vrai AutomationPipeServer cote UI.
            try
            {
                await HandleConnectionAsync(server, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"ControlPipeServer: erreur de connexion ignoree ({ex.Message})");
            }
        }
    }

    private async Task HandleConnectionAsync(NamedPipeServerStream server, CancellationToken cancellationToken)
    {
        using var reader = new StreamReader(server, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
        using var writer = new StreamWriter(server, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };

        string? line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
        if (line is null || line.Trim().Length == 0)
        {
            return;
        }

        string responseJson = HandleLine(line);
        await writer.WriteLineAsync(responseJson).ConfigureAwait(false);
    }

    private string HandleLine(string line)
    {
        JsonObject? request;
        try
        {
            request = JsonNode.Parse(line) as JsonObject;
        }
        catch (JsonException ex)
        {
            return WriteError(null, $"JSON invalide : {ex.Message}");
        }

        if (request is null)
        {
            return WriteError(null, "JSON invalide : objet attendu.");
        }

        JsonNode? id = request.TryGetPropertyValue("id", out var idNode) ? idNode : null;

        string method = request.TryGetPropertyValue("method", out var methodNode) ? (methodNode?.GetValue<string>() ?? "") : "";
        if (string.IsNullOrEmpty(method))
        {
            return WriteError(id, "Champ \"method\" manquant ou vide.");
        }

        JsonArray paramsArray = request.TryGetPropertyValue("params", out var paramsNode) && paramsNode is JsonArray arr
            ? arr
            : new JsonArray();

        try
        {
            object? result = _dispatcher.Invoke(method, paramsArray);
            return WriteResult(id, result);
        }
        catch (MethodDispatchException ex)
        {
            return WriteError(id, ex.Message);
        }
        catch (Exception ex)
        {
            return WriteError(id, $"Exception non geree dans {method} : {ex.Message}");
        }
    }

    private static string WriteResult(JsonNode? id, object? result)
    {
        var response = new JsonObject();
        if (id is not null) response["id"] = id.DeepClone();
        response["result"] = JsonSerializer.SerializeToNode(result, result?.GetType() ?? typeof(object));
        return response.ToJsonString();
    }

    private static string WriteError(JsonNode? id, string message)
    {
        var response = new JsonObject();
        if (id is not null) response["id"] = id.DeepClone();
        response["error"] = message;
        return response.ToJsonString();
    }
}

public sealed class MethodDispatchException : Exception
{
    public MethodDispatchException(string message) : base(message) { }
}

using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

namespace KillEngine.ClrInspector.Tests;

/// <summary>
/// Client JSON-RPC minimal pour les named pipes KillEngine (meme protocole
/// que scripts/automation-pipe-call.ps1, apps/desktop/automation_pipe_server.h,
/// et les deux ControlPipeServer.cs des projets .NET) : une ligne = une
/// requete/reponse, connexion fermee apres. Reimplemente ici en C# plutot que
/// d'invoquer le script PowerShell depuis les tests -- plus rapide, plus
/// fiable en execution automatisee (pas de dependance a powershell.exe ni de
/// parsing de sortie console).
/// </summary>
public static class PipeClient
{
    public static async Task<JsonNode?> CallAsync(
        string pipeName,
        string method,
        JsonArray? paramsArray = null,
        int connectTimeoutMs = 5000,
        CancellationToken cancellationToken = default)
    {
        using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        await client.ConnectAsync(connectTimeoutMs, cancellationToken).ConfigureAwait(false);

        using var writer = new StreamWriter(client, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        using var reader = new StreamReader(client, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);

        var request = new JsonObject
        {
            ["id"] = Random.Shared.Next(1, 1_000_000),
            ["method"] = method,
            ["params"] = paramsArray ?? new JsonArray(),
        };

        await writer.WriteLineAsync(request.ToJsonString()).ConfigureAwait(false);

        string? line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
        if (line is null)
        {
            throw new InvalidOperationException($"Pipe '{pipeName}' : pas de reponse pour la methode '{method}'.");
        }

        JsonNode? response = JsonNode.Parse(line);
        if (response is JsonObject obj && obj.TryGetPropertyValue("error", out var errorNode) && errorNode is not null)
        {
            throw new InvalidOperationException($"Pipe '{pipeName}', methode '{method}' : erreur distante -- {errorNode.GetValue<string>()}");
        }

        return response is JsonObject responseObj && responseObj.TryGetPropertyValue("result", out var resultNode)
            ? resultNode
            : null;
    }

    /// <summary>Polle une connexion pipe jusqu'a succes ou timeout -- utile juste apres le lancement d'un process, le temps que son serveur pipe demarre.</summary>
    public static async Task WaitForPipeReadyAsync(string pipeName, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        Exception? lastError = null;
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                await CallAsync(pipeName, "ping", connectTimeoutMs: 500).ConfigureAwait(false);
                return;
            }
            catch (Exception ex)
            {
                lastError = ex;
                await Task.Delay(200).ConfigureAwait(false);
            }
        }
        throw new TimeoutException($"Pipe '{pipeName}' non pret apres {timeout.TotalSeconds}s.", lastError);
    }
}

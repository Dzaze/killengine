using System.Diagnostics;

namespace KillEngine.ClrInspector.Tests;

/// <summary>
/// Localise le binaire construit d'un projet .NET voisin (Debug ou Release,
/// n'importe lequel des deux tant qu'il existe) sans dependre d'un chemin
/// absolu fige -- ces tests peuvent tourner depuis une machine ou seul l'un
/// des deux a ete construit.
/// </summary>
public static class BuiltAssemblyLocator
{
    public static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "AGENTS.md")))
        {
            dir = dir.Parent;
        }
        if (dir is null)
        {
            throw new InvalidOperationException(
                "Racine du depot introuvable (AGENTS.md absent en remontant depuis " + AppContext.BaseDirectory + ").");
        }
        return dir.FullName;
    }

    public static string FindDll(string projectRelativeDir, string assemblyFileName)
    {
        string repoRoot = FindRepoRoot();
        string binDir = Path.Combine(repoRoot, projectRelativeDir, "bin");
        if (!Directory.Exists(binDir))
        {
            throw new FileNotFoundException(
                $"'{binDir}' introuvable -- construire d'abord le projet (scripts/build-clr-test-target.ps1, ou 'dotnet build' dans {projectRelativeDir}).");
        }

        // Prefere Release si les deux configurations existent.
        foreach (var config in new[] { "Release", "Debug" })
        {
            var candidate = Path.Combine(binDir, config, "net8.0", assemblyFileName);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException(
            $"'{assemblyFileName}' introuvable sous '{binDir}' (Release ou Debug) -- construire d'abord le projet.");
    }
}

/// <summary>
/// Lance un executable .NET (via `dotnet <dll>`) et attend que son pipe de
/// controle reponde avant de rendre la main. Tue le process au Dispose,
/// meme si un test echoue en cours de route -- ne doit jamais laisser un
/// KillEngineClrTestTarget.exe/KillEngineClrInspector.exe orphelin tourner
/// apres une suite de tests.
/// </summary>
public sealed class ManagedProcessFixture : IAsyncDisposable
{
    public Process Process { get; }
    public int Pid => Process.Id;
    public string PipeName { get; }

    private ManagedProcessFixture(Process process, string pipeName)
    {
        Process = process;
        PipeName = pipeName;
    }

    public static async Task<ManagedProcessFixture> StartAsync(
        string dllPath,
        string pipeName,
        TimeSpan readyTimeout,
        IReadOnlyDictionary<string, string>? environment = null)
    {
        var psi = new ProcessStartInfo
        {
            FileName = "dotnet",
            Arguments = $"\"{dllPath}\"",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        if (environment is not null)
        {
            foreach (var (key, value) in environment)
            {
                psi.Environment[key] = value;
            }
        }

        var process = Process.Start(psi) ?? throw new InvalidOperationException($"Echec de lancement de '{dllPath}'.");

        try
        {
            await PipeClient.WaitForPipeReadyAsync(pipeName, readyTimeout).ConfigureAwait(false);
        }
        catch
        {
            TryKill(process);
            throw;
        }

        return new ManagedProcessFixture(process, pipeName);
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            await PipeClient.CallAsync(PipeName, "shutdown", connectTimeoutMs: 1000).ConfigureAwait(false);
            if (!Process.WaitForExit(3000))
            {
                TryKill(Process);
            }
        }
        catch
        {
            TryKill(Process);
        }
        finally
        {
            Process.Dispose();
        }
    }

    private static void TryKill(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch
        {
            // best-effort -- ne doit jamais faire echouer le teardown d'un test
        }
    }
}

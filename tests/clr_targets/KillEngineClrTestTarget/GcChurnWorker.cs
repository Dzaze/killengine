namespace KillEngine.ClrTestTarget;

/// <summary>
/// Genere en continu de la pression memoire (allocations courtes, aussitot
/// abandonnees) pour forcer des cycles de GC Gen0/Gen1/Gen2 reguliers pendant
/// que TestRoot.RootPlayer reste vivant. Objectif : que l'objet racine (et
/// son graphe) soit deplace par le compactage du GC au moins une fois pendant
/// une session de test, condition necessaire pour valider qu'un futur
/// outillage ClrMD retrouve le meme objet logique malgre le changement
/// d'adresse (docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md, exigence "survie a un
/// cycle de GC").
/// </summary>
public sealed class GcChurnWorker
{
    private volatile int _objectsPerSecond;
    private Thread? _thread;
    private volatile bool _running;

    public GcChurnWorker(int initialObjectsPerSecond)
    {
        _objectsPerSecond = Math.Max(0, initialObjectsPerSecond);
    }

    public int ObjectsPerSecond
    {
        get => _objectsPerSecond;
        set => _objectsPerSecond = Math.Max(0, value);
    }

    public long TotalAllocated { get; private set; }

    public void Start()
    {
        if (_running) return;
        _running = true;
        _thread = new Thread(Loop) { IsBackground = true, Name = "GcChurnWorker" };
        _thread.Start();
    }

    public void Stop()
    {
        _running = false;
        _thread?.Join(2000);
    }

    private void Loop()
    {
        long allocated = 0;
        var lastTick = Environment.TickCount64;

        while (_running)
        {
            int rate = _objectsPerSecond;
            if (rate <= 0)
            {
                Thread.Sleep(50);
                continue;
            }

            // Petits objets varies en taille pour peupler des tranches
            // d'allocation differentes (pas un seul type/taille repete a
            // l'identique, plus proche d'un vrai churn applicatif).
            for (int i = 0; i < rate / 10; i++)
            {
                _ = new byte[16 + (i % 7) * 32];
                _ = new ChurnRecord(i, DateTime.UtcNow.Ticks);
                allocated += 2;
            }

            TotalAllocated = allocated;

            var now = Environment.TickCount64;
            var elapsed = now - lastTick;
            var targetIntervalMs = 100; // 10 rafales/seconde
            var sleepMs = targetIntervalMs - elapsed;
            if (sleepMs > 0) Thread.Sleep((int)sleepMs);
            lastTick = Environment.TickCount64;
        }
    }

    private sealed class ChurnRecord
    {
        public ChurnRecord(int index, long ticks)
        {
            Index = index;
            Ticks = ticks;
        }

        public int Index { get; }
        public long Ticks { get; }
    }
}

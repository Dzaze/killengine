using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace KillEngine.ClrTestTarget;

// ---------------------------------------------------------------------------
// Graphe d'objets manages connus, expose pour un futur outillage ClrMD/SOS
// (docs/POWER_UP_ROADMAP.md candidat #8). Chaque type couvre une dimension
// demandee par le cahier des charges (docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md) :
//   - champs primitifs de tailles/types varies (Item, Player)
//   - references entre objets (Player.Inventory, Player.Self)
//   - collections (List<T>, tableau, Dictionary<K,V> dans Inventory)
//   - objet volontairement auto-reference (Player.Self), miroir du pattern
//     rencontre pendant l'investigation Solitaire qui a motive ce chantier
// ---------------------------------------------------------------------------

public sealed class Item
{
    public string Name = string.Empty;
    public int Value;
    public double Weight;
}

// Chantier 2 (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md, "Resolution recursive
// des structs imbriques dans un struct") : PlayerStats.HomeZone est un struct
// (Zone) qui contient lui-meme un struct (Coordinates) -- deux niveaux
// d'imbrication en plus du struct racine PlayerStats, pour verifier le
// deballage recursif borne jusqu'a la feuille primitive.
public struct Coordinates
{
    public int X;
    public int Y;
}

public struct Zone
{
    public Coordinates Origin;
    public int Radius;
}

public struct PlayerStats
{
    public int Rank;
    public float Luck;
    public Zone HomeZone;
}

public sealed class Inventory
{
    public List<Item> Items { get; } = new();
    public Item?[] QuickSlots { get; } = new Item?[4];
    public Dictionary<string, int> Currencies { get; } = new();
    public CustomBag<Item> CustomItems { get; } = new();

    // Chantier 1 (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md, "Plus de collections
    // BCL dans le deballage") : couverture HashSet<T>/Queue<T>/Stack<T> et
    // tableau multidimensionnel, en plus des List<T>/tableau 1D/Dictionary<K,V>
    // deja couverts.
    public HashSet<string> Tags { get; } = new();
    public Queue<Item> ItemQueue { get; } = new();
    public Stack<Item> ItemStack { get; } = new();
    public int[,] Grid { get; } = new int[3, 4];

    // Chantier "ecriture indexee dans des tableaux de STRUCTS" (docs/
    // KILLENGINE_CLR_INSPECTOR_SPEC.md) : tableau d'elements STRUCT (pas de
    // references, pas de primitifs) -- Coordinates est deja utilise ailleurs
    // dans ce graphe comme struct imbriquee (PlayerStats.HomeZone.Origin),
    // reutilise ici tel quel pour un tableau d'elements struct plutot que
    // d'introduire un nouveau type dedie.
    public Coordinates[] Waypoints = new Coordinates[3];

    // Chantier "struct-dans-struct-dans-tableau en ecriture" (docs/
    // POWER_UP_ROADMAP.md, "Extensions futures non bloquantes") : Zone
    // reutilise le meme struct imbrique que PlayerStats.HomeZone (Zone
    // contient lui-meme Coordinates), mais ici comme ELEMENT d'un tableau --
    // Zones[i].Origin.X est donc un champ primitif a DEUX niveaux de struct
    // sous l'element de tableau (tableau -> Zone -> Coordinates -> X),
    // un niveau de plus que Waypoints[i].X ci-dessus.
    public Zone[] Zones = new Zone[2];

    // Chantier 2 (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md, "LinkedList<T> et
    // SortedDictionary<K,V>/SortedSet<T> dans le deballage") : layout interne
    // verifie par attache ClrMD reelle avant d'ecrire le code de deballage,
    // meme methodologie que HashSet<T>/Queue<T>/Stack<T> ci-dessus.
    public LinkedList<string> LinkedTags { get; } = new();
    public SortedDictionary<string, int> SortedCurrencies { get; } = new();
    public SortedSet<int> SortedScores { get; } = new();

    // Chantier "collections concurrentes" (docs/POWER_UP_ROADMAP.md candidat
    // #8, extension listee "non couverte a ce jour") : layout interne verifie
    // par attache ClrMD reelle avant d'ecrire le code de deballage, meme
    // methodologie que HashSet<T>/Queue<T>/Stack<T>/LinkedList<T> ci-dessus.
    public ConcurrentDictionary<string, int> ConcurrentCounters { get; } = new();
    public ConcurrentStack<string> ConcurrentTags { get; } = new();
    public ConcurrentQueue<string> ConcurrentEvents { get; } = new();
    public ConcurrentBag<string> ConcurrentTraces { get; } = new();

    // Chantier "vrai plus-court-chemin GCRoot" (BFS multi-source,
    // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) : depart d'une chaine LONGUE
    // (4 sauts au total depuis le root StrongHandle qui pointe sur cette
    // Inventory) vers ShortestPathProbe -- assignee dans TestRoot.BuildGraph.
    // Un second root INDEPENDANT (TestRoot.ShortestPathShortcutHandle)
    // atteint le MEME ShortestPathProbe en seulement 1 saut. Sert a prouver
    // que findGcRootPath retourne bien le plus court des deux, pas
    // seulement "un" chemin choisi par l'ordre d'enumeration des roots.
    public ShortestPathChainNode? LongChainStart;
}

public sealed class CustomBag<T>
{
    private T?[] _items = new T?[8];
    private int _size;

    public int Count => _size;

    public void Add(T item)
    {
        _items[_size++] = item;
    }
}

// Chantier "vrai plus-court-chemin GCRoot" -- voir Inventory.LongChainStart
// et TestRoot.ShortestPathShortcutHandle pour le detail des deux chemins de
// longueurs differentes vers la meme instance de ShortestPathProbe.
public sealed class ShortestPathProbe
{
    public string Marker = "shortest-path-leaf";
}

public sealed class ShortestPathChainNode
{
    public ShortestPathChainNode? Next;
    public ShortestPathProbe? Leaf;
}

public sealed class ShortestPathShortcut
{
    public ShortestPathProbe? Target;
}

public sealed class Player
{
    public string Name = string.Empty;
    public int Health;
    public long Experience;
    public float Stamina;
    public bool IsAlive;
    public PlayerStats Stats;
    public Inventory Inventory = new();

    // Auto-reference intentionnelle : miroir du pattern observe dans l'objet
    // natif Solitaire (pointeur qui se pointe lui-meme a +0x18) qui avait ete
    // pris a tort pour un indice CLR. Ici c'est un vrai objet CLR avec un
    // vrai auto-reference geree par le GC -- sert a valider qu'un futur
    // outillage ClrMD resout correctement un cycle plutot que de boucler.
    public Player? Self;

    // ------------------------------------------------------------------
    // Propriete avec un VRAI setter (logique metier au-dela d'un simple
    // stockage de champ backing) -- dediee au chantier "appel de setter
    // reel via shellcode" (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md,
    // ResolveInstanceMethodAddress). Le setter :
    //   1. clamp la valeur dans [0, VitalityMax] (une ecriture memoire brute
    //      du champ backing _vitality ne respecterait jamais ce clamp) ;
    //   2. incremente _vitalityChangeCount, un compteur totalement distinct
    //      du champ backing -- preuve independante que le VRAI setter a
    //      tourne et pas seulement une ecriture de _vitality ;
    //   3. synchronise IsAlive a false quand la vitalite tombe a 0.
    // Champs prives explicites (pas d'auto-propriete) pour eviter tout nom
    // de champ backing genere par le compilateur (<Vitality>k__BackingField)
    // qui pourrait dérouter un test lisant les champs bruts via ClrMD.
    // ------------------------------------------------------------------
    public const int VitalityMax = 999;
    private int _vitality;
    private int _vitalityChangeCount;

    public int Vitality
    {
        get => _vitality;
        set
        {
            int clamped = value < 0 ? 0 : (value > VitalityMax ? VitalityMax : value);
            _vitality = clamped;
            _vitalityChangeCount++;
            if (clamped == 0)
            {
                IsAlive = false;
            }
        }
    }

    public int VitalityChangeCount => _vitalityChangeCount;

    // ------------------------------------------------------------------
    // Propriete jumelle de Vitality mais a parametre DOUBLE -- dediee au
    // chantier "setters float/double" (PHASE 59,
    // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md). La convention d'appel x64
    // Windows passe ce 2e argument en XMM1, pas RDX comme Vitality (int) --
    // meme genre de logique metier (clamp + compteur separe) pour prouver
    // que le VRAI setter tourne, pas juste une ecriture brute du champ
    // backing _vigor.
    // ------------------------------------------------------------------
    public const double VigorMax = 100.0;
    private double _vigor;
    private int _vigorChangeCount;

    public double Vigor
    {
        get => _vigor;
        set
        {
            double clamped = value < 0.0 ? 0.0 : (value > VigorMax ? VigorMax : value);
            _vigor = clamped;
            _vigorChangeCount++;
        }
    }

    public int VigorChangeCount => _vigorChangeCount;

    // ------------------------------------------------------------------
    // Propriete a parametre OBJET (type reference, pas primitif) -- dediee
    // au chantier "setters a parametre objet/string" (docs/
    // KILLENGINE_CLR_INSPECTOR_SPEC.md). Le setter assigne une reference
    // Item DEJA EXISTANTE sur le tas (pas de nouvelle allocation, hors
    // scope de ce chantier) et met a jour un compteur/flag separe du champ
    // backing -- meme discipline de preuve que Vitality/Vigor : la logique
    // metier (pas juste une ecriture brute de _equippedItem) doit tourner.
    // ------------------------------------------------------------------
    private Item? _equippedItem;
    private int _equipChangeCount;

    public Item? EquippedItem
    {
        get => _equippedItem;
        set
        {
            _equippedItem = value;
            _equipChangeCount++;
            IsArmed = value is not null;
        }
    }

    public int EquipChangeCount => _equipChangeCount;
    public bool IsArmed;

    // ------------------------------------------------------------------
    // Propriete a parametre STRUCT (value type, ni classe ni primitif) --
    // dediee au chantier "setters a parametre struct" (docs/
    // POWER_UP_ROADMAP.md candidat #8, extension listee "non couverte a ce
    // jour"). Convention d'appel x64 Windows pour un struct PASSE PAR
    // VALEUR : tient dans un seul registre (RDX) quand sa taille totale est
    // exactement 1/2/4/8 octets, sinon passe par un pointeur cache vers une
    // copie -- seul le premier cas (registre) est couvert par ce chantier.
    // Coordinates {int X; int Y;} fait exactement 8 octets (cas le plus
    // simple pour prouver le mecanisme, pas de trou d'alignement). Meme
    // discipline de preuve que Vitality/Vigor/EquippedItem : clamp + compteur
    // separe du champ backing, pour distinguer un VRAI appel de setter d'une
    // simple ecriture memoire brute.
    // ------------------------------------------------------------------
    private Coordinates _waypoint;
    private int _waypointChangeCount;

    public Coordinates Waypoint
    {
        get => _waypoint;
        set
        {
            int clampedX = value.X < 0 ? 0 : (value.X > 100 ? 100 : value.X);
            int clampedY = value.Y < 0 ? 0 : (value.Y > 100 ? 100 : value.Y);
            _waypoint = new Coordinates { X = clampedX, Y = clampedY };
            _waypointChangeCount++;
        }
    }

    public int WaypointChangeCount => _waypointChangeCount;

    // Tableau de primitifs (int[]) accessible depuis Player -- dedie au
    // chantier "ecriture directe par index dans un tableau primitif"
    // (PHASE 59). writePrimitivePath doit pouvoir ecrire Scores[i]
    // directement dans le tableau, pas seulement le lire.
    public int[] Scores = new int[4];

    // Propriete jumelle jamais appelee par ce process (aucun warmup dans
    // BuildGraph ci-dessous) -- dediee au test de regression "setter jamais
    // JITte" (ClrMethod.NativeCode vaut alors ulong.MaxValue, pas 0, cote
    // ClrMD reel -- verifie par reflexion avant d'ecrire ResolveInstanceMethodAddress,
    // voir ClrSession.cs). ResolveInstanceMethodAddress doit renvoyer
    // l'erreur claire documentee plutot que de tenter de forcer le JIT.
    private int _neverCalledStat;
    public int NeverCalledStat
    {
        get => _neverCalledStat;
        set => _neverCalledStat = value;
    }

    // Methode statique dediee au test de regression "setter statique
    // rejete" (ResolveInstanceMethodAddress doit refuser toute methode
    // statique -- "this" en RCX n'a pas de sens pour un appel static).
    // Corps volontairement vide : seule sa presence dans ClrType.Methods
    // (avec l'attribut Static) importe pour ce test.
    public static void StaticProbe(int value) { }
}

/// <summary>
/// Objet jetable dedie au test de regression "collecte reelle" (distinguer
/// un objet deplace par un GC compactant d'un objet reellement devenu
/// inatteignable et collecte). Type dedie, jamais confondu avec le bruit
/// genere par GcChurnWorker (ChurnRecord) ni avec le graphe principal.
/// </summary>
public sealed class DisposableProbe
{
    public int Id;
    public string Tag = string.Empty;
}

/// <summary>
/// Racine unique du graphe de test, garde vivante par un champ static (root
/// GC de type "static handle") pendant toute la duree de vie du process.
/// </summary>
public static class TestRoot
{
    public static readonly Player RootPlayer = BuildGraph();

    // Root GC de type "handle" distinct du root "static field" ci-dessus --
    // deux mecanismes de root differents que ClrMD doit savoir enumerer tous
    // les deux (ClrRuntime.EnumerateHandles vs racines statiques classiques).
    public static readonly GCHandle RootHandle = GCHandle.Alloc(RootPlayer.Inventory, GCHandleType.Normal);

    // Chantier "vrai plus-court-chemin GCRoot" (BFS multi-source,
    // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) : SECOND root StrongHandle
    // INDEPENDANT de RootHandle ci-dessus, pointant sur un objet
    // ShortestPathShortcut qui reference EN 1 SEUL SAUT la MEME instance de
    // ShortestPathProbe que celle atteinte en 4 SAUTS depuis
    // RootHandle -> Inventory -> LongChainStart -> Next -> Next -> Leaf.
    // Initialise APRES RootPlayer (l'ordre textuel des initialiseurs de
    // champs static garantit que RootPlayer.Inventory.LongChainStart est
    // deja peuple par BuildGraph() au moment ou cette ligne s'execute).
    public static readonly GCHandle ShortestPathShortcutHandle = GCHandle.Alloc(
        new ShortestPathShortcut { Target = RootPlayer.Inventory.LongChainStart!.Next!.Next!.Leaf },
        GCHandleType.Normal);

    // Mutable (pas readonly) volontairement : seul champ du graphe de test
    // qu'on doit pouvoir vider a la demande pour rendre un objet reellement
    // inatteignable, condition necessaire au test "collecte reelle" ci-dessus.
    // Jamais reference ailleurs dans le graphe -- le nullifier suffit a le
    // rendre eligible au GC des la prochaine collecte.
    public static DisposableProbe? DisposableSlot;
    private static int s_nextDisposableId = 1;

    public static DisposableProbe SpawnDisposable(string tag)
    {
        var probe = new DisposableProbe { Id = s_nextDisposableId++, Tag = tag };
        DisposableSlot = probe;
        return probe;
    }

    public static void DropDisposable() => DisposableSlot = null;

    private static Player BuildGraph()
    {
        var inventory = new Inventory();

        var sword = new Item { Name = "Sword", Value = 150, Weight = 3.5 };
        var shield = new Item { Name = "Shield", Value = 90, Weight = 6.0 };
        var potion = new Item { Name = "Potion", Value = 10, Weight = 0.3 };

        inventory.Items.Add(sword);
        inventory.Items.Add(shield);
        inventory.Items.Add(potion);
        inventory.CustomItems.Add(sword);
        inventory.CustomItems.Add(potion);

        inventory.QuickSlots[0] = sword;
        inventory.QuickSlots[1] = potion;

        inventory.Currencies["gold"] = 4125;
        inventory.Currencies["gems"] = 12;

        // "temp" est ajoute PUIS retire expres : force une vraie entree
        // libre (free-list) dans le buffer interne _entries du HashSet<T>,
        // condition necessaire pour verifier concretement que le deballage
        // ignore les entrees supprimees plutot que de les rapporter comme
        // valeurs valides (voir investigation dans
        // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md : contrairement a
        // Dictionary<K,V>, HashSet<T>.Entry.HashCode n'est PAS masque a une
        // valeur non-negative -- un hash naturellement negatif est un
        // element valide, la detection d'entree libre doit se faire
        // autrement).
        inventory.Tags.Add("common");
        inventory.Tags.Add("starter");
        inventory.Tags.Add("temp");
        inventory.Tags.Add("verified");
        inventory.Tags.Remove("temp");

        // Enqueue/Dequeue/Enqueue delibere pour forcer un vrai wraparound du
        // buffer circulaire interne (_head > 0 et _tail qui revient a 0) --
        // condition necessaire pour verifier que le deballage de Queue<T>
        // gere correctement l'indexation circulaire, pas seulement le cas
        // trivial _head == 0. Contenu logique final (avant -> arriere) :
        // shield, potion, sword.
        inventory.ItemQueue.Enqueue(sword);
        inventory.ItemQueue.Enqueue(shield);
        inventory.ItemQueue.Enqueue(potion);
        inventory.ItemQueue.Dequeue();
        inventory.ItemQueue.Enqueue(sword);

        // Push/Pop/Push : contenu logique final (bas -> haut) : potion, shield.
        inventory.ItemStack.Push(potion);
        inventory.ItemStack.Push(sword);
        inventory.ItemStack.Pop();
        inventory.ItemStack.Push(shield);

        for (int row = 0; row < inventory.Grid.GetLength(0); row++)
        {
            for (int col = 0; col < inventory.Grid.GetLength(1); col++)
            {
                inventory.Grid[row, col] = row * 10 + col;
            }
        }

        inventory.Waypoints[0] = new Coordinates { X = 1, Y = 1 };
        inventory.Waypoints[1] = new Coordinates { X = 2, Y = 2 };
        inventory.Waypoints[2] = new Coordinates { X = 3, Y = 3 };

        inventory.Zones[0] = new Zone { Origin = new Coordinates { X = 100, Y = 200 }, Radius = 5 };
        inventory.Zones[1] = new Zone { Origin = new Coordinates { X = 300, Y = 400 }, Radius = 10 };

        // Chantier 2 (LinkedList<T>/SortedDictionary<K,V>/SortedSet<T>) :
        // sequence deliberement en DESORDRE d'allocation pour prouver que
        // readObject restitue l'ORDRE LOGIQUE (pas l'ordre d'allocation
        // memoire). AddFirst/AddLast/Remove pour LinkedList<T> ; insertions
        // desordonnees pour les deux collections triees.
        inventory.LinkedTags.AddLast("second");
        inventory.LinkedTags.AddLast("temp-to-remove");
        inventory.LinkedTags.AddFirst("first");
        inventory.LinkedTags.AddLast("third");
        inventory.LinkedTags.Remove("temp-to-remove");
        // Ordre logique final attendu : first, second, third.

        inventory.SortedCurrencies["silver"] = 500;
        inventory.SortedCurrencies["copper"] = 9000;
        inventory.SortedCurrencies["gold"] = 12;
        // Ordre logique trie par cle attendu : copper, gold, silver.

        inventory.SortedScores.Add(42);
        inventory.SortedScores.Add(7);
        inventory.SortedScores.Add(99);
        inventory.SortedScores.Add(15);
        // Ordre logique trie attendu : 7, 15, 42, 99.

        // "stale" est ajoute PUIS retire expres, "hits" est ajoute PUIS mis a
        // jour via TryUpdate (pas juste []=) : force une vraie entree
        // supprimee ET une entree dont le noeud interne a ete remplace, pas
        // seulement des insertions vierges -- condition necessaire pour
        // verifier concretement que le deballage de ConcurrentDictionary<K,V>
        // ignore les tombstones/buckets vides plutot que de les rapporter.
        inventory.ConcurrentCounters["hits"] = 1;
        inventory.ConcurrentCounters["stale"] = 999;
        inventory.ConcurrentCounters.TryUpdate("hits", 41, 1);
        inventory.ConcurrentCounters["misses"] = 7;
        inventory.ConcurrentCounters.TryRemove("stale", out _);
        // Contenu logique final attendu (ordre non garanti par la structure
        // elle-meme) : hits=41, misses=7.

        // ConcurrentStack<T> : Push/Pop delibere pour verifier que le
        // deballage suit la chaine _head -> _next en ordre logique "sommet
        // d'abord", sans rapporter l'element retire.
        inventory.ConcurrentTags.Push("bottom");
        inventory.ConcurrentTags.Push("temp-to-pop");
        inventory.ConcurrentTags.TryPop(out _);
        inventory.ConcurrentTags.Push("middle");
        inventory.ConcurrentTags.Push("top");
        // Ordre logique attendu : top, middle, bottom.

        // ConcurrentQueue<T> : capacite initiale de segment = 32 (verifie par
        // reflection sur le runtime local avant d'ecrire le code de
        // deballage), donc 40 Enqueue force un deuxieme segment ; dequeue de
        // 35 elements epuise completement le premier segment ET mange une
        // partie du second -- exerce deliberement la traversee de frontiere
        // de segment, pas seulement le cas a un seul segment.
        for (int i = 0; i < 40; i++)
        {
            inventory.ConcurrentEvents.Enqueue($"evt-{i}");
        }
        for (int i = 0; i < 35; i++)
        {
            inventory.ConcurrentEvents.TryDequeue(out _);
        }
        // Contenu logique final attendu (ordre FIFO garanti) : evt-35..evt-39.

        // ConcurrentBag<T> : structure "work-stealing" a affinite de thread
        // (layout verifie par reflection sur le runtime local puis valide
        // empiriquement contre ToArray() sur 10 scenarios avant d'ecrire le
        // code de deballage, voir le commentaire de ClrSession.DescribeConcurrentBag).
        // Un thread producteur ajoute 6 elements PUIS meurt (Join) -- sa file
        // interne (WorkStealingQueue) reste dans la liste chainee du bag mais
        // avec un proprietaire mort. Le thread principal n'a pas de file
        // locale dans ce bag, donc ses TryTake suivants VOLENT depuis la file
        // du thread mort (FIFO, retire par le DEBUT) plutot que de faire un
        // pop local LIFO -- exerce deliberement le cas ou _headIndex avance
        // au-dela de 0 sur une file dont le proprietaire n'existe plus, pas
        // seulement le cas a un seul thread jamais vole.
        var producer = new Thread(() =>
        {
            for (int i = 0; i < 6; i++)
            {
                inventory.ConcurrentTraces.Add($"trace-{i}");
            }
        });
        producer.Start();
        producer.Join();
        inventory.ConcurrentTraces.TryTake(out _);
        inventory.ConcurrentTraces.TryTake(out _);
        // Contenu logique final attendu (ordre "tail d'abord", trace-5 = le
        // plus recemment ajoute encore present) : trace-5, trace-4, trace-3, trace-2.

        // Chantier "vrai plus-court-chemin GCRoot" : chaine de 3 noeuds vers
        // ShortestPathProbe, soit 4 sauts au total depuis le root
        // StrongHandle qui pointe sur cette Inventory (RootHandle ci-dessous,
        // Inventory = objet racine a profondeur 0, LongChainStart = 1, Next =
        // 2, Next = 3, Leaf = 4). TestRoot.ShortestPathShortcutHandle
        // referencera plus bas la MEME instance de probe en seulement 1 saut
        // depuis un root distinct -- voir la doc sur le champ.
        var shortestPathProbe = new ShortestPathProbe();
        inventory.LongChainStart = new ShortestPathChainNode
        {
            Next = new ShortestPathChainNode
            {
                Next = new ShortestPathChainNode { Leaf = shortestPathProbe },
            },
        };

        var player = new Player
        {
            Name = new string("TestSubject".ToCharArray()),
            Health = 100,
            Experience = 5000,
            Stamina = 75.0f,
            IsAlive = true,
            Stats = new PlayerStats
            {
                Rank = 7,
                Luck = 1.25f,
                HomeZone = new Zone { Origin = new Coordinates { X = 12, Y = -4 }, Radius = 30 },
            },
            Inventory = inventory,
            Scores = new[] { 10, 20, 30, 40 },
        };
        player.Self = player;

        // Warmup deliberement unique (pas une boucle) : force le JIT du
        // setter reel (set_Vitality) des le demarrage du process, condition
        // necessaire pour que ResolveInstanceMethodAddress (ClrMD) trouve un
        // NativeCode != 0 des l'attache -- un seul appel reste tres en
        // dessous du seuil de bascule tiered compilation (~30 appels), donc
        // le code natif Tier0 reste stable pour toute la duree du process.
        player.Vitality = 500;

        // Warmup du setter a parametre double (set_Vigor), meme raison que
        // ci-dessus -- necessaire pour PHASE 59 (setters float/double).
        player.Vigor = 42.5;

        // Warmup du setter a parametre objet (set_EquippedItem), meme raison
        // -- chantier "setters a parametre objet/string". La valeur du
        // warmup elle-meme n'a pas d'importance (le test dedie appelle ce
        // setter avec une adresse d'objet reelle par shellcode), seul le
        // fait qu'il tourne au moins une fois avant l'attache compte.
        player.EquippedItem = sword;

        // Warmup du setter a parametre STRUCT (set_Waypoint), meme raison --
        // chantier "setters a parametre struct".
        player.Waypoint = new Coordinates { X = 1, Y = 1 };

        return player;
    }

    /// <summary>
    /// Identite stable independante de l'adresse memoire : le hashcode par
    /// defaut de System.Object est fige dans le sync block header au premier
    /// appel et survit a un déplacement du GC (contrairement a l'adresse
    /// elle-meme). Sert a verifier qu'un objet retrouve apres un cycle de GC
    /// est bien "le meme" logiquement, independamment de sa nouvelle adresse.
    /// </summary>
    public static int StableIdentityOf(object obj) => RuntimeHelpers.GetHashCode(obj);
}

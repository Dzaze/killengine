using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

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

    // Chantier 2 (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md, "LinkedList<T> et
    // SortedDictionary<K,V>/SortedSet<T> dans le deballage") : layout interne
    // verifie par attache ClrMD reelle avant d'ecrire le code de deballage,
    // meme methodologie que HashSet<T>/Queue<T>/Stack<T> ci-dessus.
    public LinkedList<string> LinkedTags { get; } = new();
    public SortedDictionary<string, int> SortedCurrencies { get; } = new();
    public SortedSet<int> SortedScores { get; } = new();
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

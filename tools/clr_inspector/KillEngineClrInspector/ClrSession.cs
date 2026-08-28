using Microsoft.Diagnostics.Runtime;
using System.Globalization;
using System.Runtime.InteropServices;

namespace KillEngine.ClrInspector;

public sealed class ClrSessionException : Exception
{
    public ClrSessionException(string message) : base(message) { }
}

public sealed record PathWriteOperation(string Path, string Value);

/// <summary>
/// Detient l'attache ClrMD active (au plus une a la fois pour ce MVP -- un
/// deuxieme "attach" remplace la precedente apres Detach implicite). API
/// exacte de Microsoft.Diagnostics.Runtime verifiee par reflexion sur
/// l'assembly reellement restauree (4.0.732401) avant d'ecrire ce fichier,
/// pas devinee de memoire -- voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md.
/// </summary>
public sealed class ClrSession : IDisposable
{
    private const int MaxCollectionItems = 32;

    // Chantier "LinkedList<T>/SortedDictionary<K,V>/SortedSet<T>" (docs/
    // KILLENGINE_CLR_INSPECTOR_SPEC.md) : borne de securite sur le nombre de
    // noeuds d'arbre visites pendant un parcours en ordre (in-order) de
    // SortedDictionary/SortedSet. Un arbre rouge-noir bien forme garantit une
    // hauteur O(log n), et le parcours iteratif s'arrete des que
    // MaxCollectionItems elements ont ete produits -- cette borne ne devrait
    // donc jamais etre atteinte en usage normal, meme sur un tres gros
    // SortedDictionary/SortedSet. Elle protege uniquement contre une
    // structure corrompue/degenerescente (pas un scenario legitime), d'ou une
    // valeur genereuse plutot que calibree.
    private const int MaxSortedTreeNodesVisited = 5_000;

    // Chantier "resolution recursive des structs imbriques" (docs/
    // KILLENGINE_CLR_INSPECTOR_SPEC.md) : profondeur maximale de deballage
    // struct-dans-struct. Borne deliberee -- un struct generique complexe
    // pourrait sinon se referencer indirectement et provoquer une recursion
    // couteuse ou une boucle (les structs .NET ne peuvent pas former de cycle
    // direct par valeur, mais un type generique imbrique profondement reste
    // possible). 4 niveaux couvre largement les cas reels (struct de struct
    // de struct), au-dela un placeholder explicite est renvoye plutot que de
    // continuer a deployer.
    private const int MaxValueTypeDepth = 4;
    private const uint ProcessVmWrite = 0x0020;
    private const uint ProcessVmOperation = 0x0008;

    private DataTarget? _dataTarget;
    private ClrRuntime? _runtime;
    private int? _attachedPid;

    public bool IsAttached => _runtime is not null;

    public object Attach(int pid)
    {
        DetachInternal();

        // AttachToProcess(suspend:false) = attache "passive" (pas
        // CreateSnapshotAndAttach) : lit la memoire live du process cible a
        // la demande, sans figer un instantane au moment de l'appel. Choix
        // deliberement different de CreateSnapshotAndAttach -- le scenario
        // central de ce MVP (retrouver le meme objet apres un forceGC
        // declenche entre deux appels pipe) exige de relire l'etat memoire
        // COURANT, pas un instantane fige avant le GC.
        DataTarget dataTarget;
        try
        {
            dataTarget = DataTarget.AttachToProcess(pid, suspend: false);
        }
        catch (Exception ex)
        {
            throw new ClrSessionException($"Attache au PID {pid} echouee : {ex.Message}");
        }

        if (dataTarget.ClrVersions.Length == 0)
        {
            dataTarget.Dispose();
            throw new ClrSessionException(
                $"Aucun CLR detecte dans le PID {pid} -- ce process n'est probablement pas une cible managee " +
                "(.NET Framework/CoreCLR). Voir docs/STRATEGY_ROOM.md pour le cas Solitaire, qui a echoue exactement ici.");
        }

        ClrInfo clrInfo = dataTarget.ClrVersions[0];
        ClrRuntime runtime;
        try
        {
            runtime = clrInfo.CreateRuntime();
        }
        catch (Exception ex)
        {
            dataTarget.Dispose();
            throw new ClrSessionException($"CreateRuntime a echoue pour le CLR detecte ({clrInfo.Flavor} {clrInfo.Version}) : {ex.Message}");
        }

        _dataTarget = dataTarget;
        _runtime = runtime;
        _attachedPid = pid;

        return new
        {
            pid,
            clrFlavor = clrInfo.Flavor.ToString(),
            clrVersion = clrInfo.Version.ToString(),
            clrVersionsFound = dataTarget.ClrVersions.Length,
        };
    }

    public object Detach()
    {
        bool wasAttached = IsAttached;
        DetachInternal();
        return new { wasAttached };
    }

    private void DetachInternal()
    {
        _runtime?.Dispose();
        _runtime = null;
        _dataTarget?.Dispose();
        _dataTarget = null;
        _attachedPid = null;
    }

    /// <summary>
    /// A appeler apres un GC declenche en dehors de cette session (ex: via
    /// le pipe de controle de KillEngineClrTestTarget) et avant de rejouer
    /// un heap walk -- invalide le cache interne de ClrMD (segments/heap
    /// layout) pour forcer une relecture fraiche de l'etat memoire live.
    /// </summary>
    public object FlushCachedData()
    {
        var runtime = RequireRuntime();
        runtime.FlushCachedData();
        return new { flushed = true };
    }

    public object FindObjectsByTypeSubstring(string typeSubstring)
    {
        var runtime = RequireRuntime();
        var heap = runtime.Heap;

        var results = new List<object>();
        foreach (ClrObject obj in heap.EnumerateObjects())
        {
            ClrType? type = obj.Type;
            if (type?.Name is null) continue;
            if (!type.Name.Contains(typeSubstring, StringComparison.Ordinal)) continue;

            results.Add(new
            {
                address = ToHex(obj.Address),
                typeName = type.Name,
                size = obj.Size,
            });
        }
        return results;
    }

    public object FindObjectsByFieldValue(string typeSubstring, string fieldName, string expectedValueText, int maxResults)
    {
        var runtime = RequireRuntime();
        var heap = runtime.Heap;
        string typeFilter = typeSubstring.Trim();
        string fieldFilter = fieldName.Trim();
        if (typeFilter.Length == 0 || fieldFilter.Length == 0)
        {
            throw new ClrSessionException("Type et champ requis pour un locator CLR.");
        }

        int limit = Math.Clamp(maxResults, 1, 200);
        var results = new List<object>();
        int scanned = 0;
        int typeMatches = 0;
        foreach (ClrObject obj in heap.EnumerateObjects())
        {
            scanned++;
            ClrType? type = obj.Type;
            if (type?.Name is null) continue;
            if (!type.Name.Contains(typeFilter, StringComparison.Ordinal)) continue;
            typeMatches++;

            ClrInstanceField? field = FindField(type, fieldFilter);
            if (field is null) continue;

            object? value;
            try
            {
                value = ReadFieldValue(obj, field);
            }
            catch
            {
                continue;
            }
            if (!FieldValueMatches(field.ElementType, value, expectedValueText))
            {
                continue;
            }

            results.Add(new
            {
                address = ToHex(obj.Address),
                typeName = type.Name,
                size = obj.Size,
                identityField = field.Name,
                identityValue = value,
            });
            if (results.Count >= limit)
            {
                break;
            }
        }

        return new
        {
            success = results.Count > 0,
            typeSubstring = typeFilter,
            fieldName = fieldFilter,
            expectedValue = expectedValueText,
            scannedObjects = scanned,
            typeMatches,
            matchesReturned = results.Count,
            maxResults = limit,
            matches = results,
        };
    }

    public object ReadObject(string addressHex)
    {
        var runtime = RequireRuntime();
        ulong address = ParseHexAddress(addressHex);

        ClrObject obj = runtime.Heap.GetObject(address);
        if (obj.IsNull || !obj.IsValid)
        {
            throw new ClrSessionException($"Adresse {addressHex} : objet invalide ou null (probablement deplace/collecte -- relire les adresses avec findObjectsByType apres un flushCachedData).");
        }

        return DescribeObject(obj);
    }

    private static object DescribeObject(ClrObject obj)
    {
        ClrType? type = obj.Type;
        if (type is null)
        {
            return new
            {
                address = ToHex(obj.Address),
                typeName = (string?)null,
                fields = new Dictionary<string, object?>(),
                fieldDetails = Array.Empty<object>(),
            };
        }

        var fields = new Dictionary<string, object?>();
        var fieldDetails = new List<object>();
        foreach (ClrInstanceField field in type.Fields)
        {
            string name = field.Name ?? "?";
            try
            {
                object? value = ReadFieldValue(obj, field);
                fields[name] = value;
                fieldDetails.Add(DescribeField(obj, field, name, value));
            }
            catch (Exception ex)
            {
                string error = $"<erreur lecture: {ex.Message}>";
                fields[name] = error;
                fieldDetails.Add(new
                {
                    name,
                    typeName = field.Type?.Name,
                    elementType = field.ElementType.ToString(),
                    kind = "error",
                    writable = false,
                    error,
                });
            }
        }

        return new
        {
            address = ToHex(obj.Address),
            typeName = type.Name,
            size = obj.Size,
            fields,
            fieldDetails,
            collection = DescribeCollection(obj, depth: 0),
        };
    }

    private static object DescribeField(ClrObject obj, ClrInstanceField field, string name, object? value)
    {
        bool writable = IsWritablePrimitive(field.ElementType);
        ulong address = 0;
        if (writable)
        {
            try
            {
                address = field.GetAddress(obj.Address);
            }
            catch
            {
                writable = false;
            }
        }

        return new
        {
            name,
            typeName = field.Type?.Name,
            elementType = field.ElementType.ToString(),
            kind = FieldKind(field),
            value,
            address = address == 0 ? null : ToHex(address),
            size = writable ? PrimitiveSize(field.ElementType) : 0,
            writable,
        };
    }

    private static object? ReadFieldValue(ClrObject obj, ClrInstanceField field, int depth = 0)
    {
        if (field.ElementType == ClrElementType.String)
        {
            return obj.ReadStringField(field.Name!, 4096);
        }

        if (field.IsPrimitive)
        {
            return field.ElementType switch
            {
                ClrElementType.Boolean => obj.ReadField<bool>(field.Name!),
                ClrElementType.Char => (int)obj.ReadField<char>(field.Name!),
                ClrElementType.Int8 => obj.ReadField<sbyte>(field.Name!),
                ClrElementType.UInt8 => obj.ReadField<byte>(field.Name!),
                ClrElementType.Int16 => obj.ReadField<short>(field.Name!),
                ClrElementType.UInt16 => obj.ReadField<ushort>(field.Name!),
                ClrElementType.Int32 => obj.ReadField<int>(field.Name!),
                ClrElementType.UInt32 => obj.ReadField<uint>(field.Name!),
                ClrElementType.Int64 => obj.ReadField<long>(field.Name!),
                ClrElementType.UInt64 => obj.ReadField<ulong>(field.Name!),
                ClrElementType.Float => obj.ReadField<float>(field.Name!),
                ClrElementType.Double => obj.ReadField<double>(field.Name!),
                _ => $"<primitif non gere: {field.ElementType}>",
            };
        }

        if (field.IsObjectReference)
        {
            ClrObject refObj = obj.ReadObjectField(field.Name!);
            if (refObj.IsNull)
            {
                return null;
            }
            // Piege reel rencontre par attache ClrMD reelle en verifiant
            // LinkedList<T>.item avant d'ecrire le chantier "LinkedList<T>/
            // SortedDictionary<K,V>/SortedSet<T>" (docs/
            // KILLENGINE_CLR_INSPECTOR_SPEC.md) : pour un champ dont le type
            // declare est un PARAMETRE GENERIQUE T instancie en reference
            // (ex: LinkedListNode<string>.item), ClrMD rapporte
            // field.ElementType == Class (partage de code generique canonique
            // cote CLR pour les types reference), PAS ElementType.String --
            // meme quand le type reel resolu de l'instance (refObj.Type.Name)
            // est bel et bien "System.String". Le check ElementType.String en
            // tete de cette methode (qui suffit pour un champ string DIRECT,
            // non generique, ex: Player.Name) ne suffit donc pas ici. Meme
            // garde-fou deja en place pour les champs struct generiques dans
            // ReadValueTypeFieldValue plus bas (HashSet<T>.Entry.Value,
            // Dictionary<K,V>.Entry.key/value, etc.) -- applique ici aussi
            // pour les champs objet directs.
            if (refObj.Type?.IsString == true)
            {
                return refObj.AsString(4096);
            }
            return DescribeObjectReference(refObj, depth: 0);
        }

        // Champ value-type (struct) autre que primitif : deballage recursif
        // borne (chantier "resolution recursive des structs imbriques",
        // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) -- reutilise DescribeValueType,
        // la meme logique deja utilisee pour un struct racine d'element de
        // collection (List<T>/tableau/Dictionary<K,V> de structs).
        if (depth < MaxValueTypeDepth && field.Type?.IsValueType == true)
        {
            ClrValueType nested = obj.ReadValueTypeField(field.Name!);
            return DescribeValueType(nested, depth + 1);
        }

        return $"<value-type non deroule (profondeur max {MaxValueTypeDepth} atteinte): {field.Type?.Name ?? field.ElementType.ToString()}>";
    }

    private static object DescribeObjectReference(ClrObject obj, int depth)
    {
        var result = new Dictionary<string, object?>
        {
            ["address"] = ToHex(obj.Address),
            ["typeName"] = obj.Type?.Name,
        };

        if (depth < 1)
        {
            object? collection = DescribeCollection(obj, depth + 1);
            if (collection is not null)
            {
                result["collection"] = collection;
            }
        }

        return result;
    }

    private static object? DescribeCollection(ClrObject obj, int depth)
    {
        ClrType? type = obj.Type;
        if (type is null) return null;

        try
        {
            if (type.IsArray)
            {
                // Tableaux multidimensionnels (ex: int[,]) : Rank > 1, verifie
                // par attache ClrMD reelle (ClrArray.Rank/GetLength(d), voir
                // docs/KILLENGINE_CLR_INSPECTOR_SPEC.md, chantier "Plus de
                // collections BCL"). Les tableaux 1D restent geres par
                // DescribeArray comme avant.
                if (obj.AsArray().Rank > 1)
                {
                    return DescribeMultiDimensionalArray(obj, depth);
                }
                return DescribeArray(obj, null, depth);
            }

            string typeName = type.Name ?? "";
            if (typeName.StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
            {
                ClrObject items = obj.ReadObjectField("_items");
                int size = SafeReadIntField(obj, "_size");
                return DescribeArray(items, size, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.Dictionary<", StringComparison.Ordinal))
            {
                return DescribeDictionary(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.HashSet<", StringComparison.Ordinal))
            {
                return DescribeHashSet(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.Queue<", StringComparison.Ordinal))
            {
                return DescribeQueue(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.Stack<", StringComparison.Ordinal))
            {
                return DescribeStack(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.LinkedList<", StringComparison.Ordinal))
            {
                return DescribeLinkedList(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.SortedDictionary<", StringComparison.Ordinal))
            {
                return DescribeSortedDictionary(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Generic.SortedSet<", StringComparison.Ordinal))
            {
                return DescribeSortedSet(obj, depth);
            }

            if (typeName.StartsWith("System.Collections.Concurrent.ConcurrentDictionary<", StringComparison.Ordinal))
            {
                return DescribeConcurrentDictionary(obj, depth);
            }

            object? customCollection = DescribeFieldBackedCollection(obj, depth);
            if (customCollection is not null)
            {
                return customCollection;
            }
        }
        catch (Exception ex)
        {
            return new { kind = "collection_error", error = ex.Message };
        }

        return null;
    }

    private static object? DescribeFieldBackedCollection(ClrObject obj, int depth)
    {
        ClrType? type = obj.Type;
        if (type is null || type.IsString)
        {
            return null;
        }

        ClrInstanceField? itemsField = FindFirstField(type, "_items", "items", "Items", "_array", "array");
        if (itemsField is null || !itemsField.IsObjectReference)
        {
            return null;
        }

        ClrObject items = obj.ReadObjectField(itemsField.Name!);
        if (items.IsNull || items.Type is null || !items.Type.IsArray)
        {
            return null;
        }

        int physicalLength = items.AsArray().GetLength(0);
        int logicalCount = physicalLength;
        ClrInstanceField? countField = FindFirstField(type, "_size", "size", "_count", "count", "Count");
        if (countField is not null && countField.ElementType == ClrElementType.Int32)
        {
            logicalCount = Math.Clamp(obj.ReadField<int>(countField.Name!), 0, physicalLength);
        }

        object collection = DescribeArray(items, logicalCount, depth);
        return new
        {
            kind = "custom_field_backed",
            typeName = type.Name,
            itemsField = itemsField.Name,
            countField = countField?.Name,
            collection,
        };
    }

    private static object DescribeArray(ClrObject arrayObject, int? logicalCount, int depth)
    {
        if (arrayObject.IsNull || arrayObject.Type is null || !arrayObject.Type.IsArray)
        {
            return new { kind = "array", count = 0, returned = 0, items = Array.Empty<object?>(), error = "tableau null ou invalide" };
        }

        ClrArray array = arrayObject.AsArray();
        int physicalLength = array.GetLength(0);
        int count = Math.Max(0, Math.Min(logicalCount ?? physicalLength, physicalLength));
        int returned = Math.Min(count, MaxCollectionItems);
        var items = new List<object?>(returned);
        ClrType? componentType = arrayObject.Type.ComponentType;

        for (int i = 0; i < returned; ++i)
        {
            items.Add(ReadArrayElement(array, componentType, i, depth));
        }

        return new
        {
            kind = logicalCount.HasValue ? "list" : "array",
            elementType = componentType?.Name,
            count,
            returned,
            truncated = count > returned,
            items,
        };
    }

    private static object? ReadArrayElement(ClrArray array, ClrType? componentType, int index, int depth)
    {
        if (componentType is null)
        {
            return null;
        }

        if (componentType.ElementType == ClrElementType.String)
        {
            ClrObject str = array.GetObjectValue(index);
            return str.IsNull ? null : str.AsString(4096);
        }

        if (IsPrimitiveElement(componentType.ElementType))
        {
            return ReadArrayPrimitive(array, componentType.ElementType, index);
        }

        if (!componentType.IsValueType)
        {
            ClrObject refObj = array.GetObjectValue(index);
            return refObj.IsNull ? null : DescribeObjectReference(refObj, depth);
        }

        ClrValueType value = array.GetStructValue(index);
        return DescribeValueType(value, depth);
    }

    private static object? DescribeDictionary(ClrObject dictionaryObject, int depth)
    {
        ClrObject entriesObject = dictionaryObject.ReadObjectField("_entries");
        if (entriesObject.IsNull)
        {
            return new { kind = "dictionary", count = 0, returned = 0, entries = Array.Empty<object?>() };
        }

        int count = Math.Max(0, SafeReadIntField(dictionaryObject, "_count"));
        int returned = Math.Min(count, MaxCollectionItems);
        ClrArray entriesArray = entriesObject.AsArray();
        int physicalLength = entriesArray.GetLength(0);
        var entries = new List<object?>(returned);

        for (int i = 0; i < physicalLength && entries.Count < returned; ++i)
        {
            ClrValueType entry = entriesArray.GetStructValue(i);
            int hashCode = TryReadValueTypeIntField(entry, "hashCode", out int hc) ? hc : 0;
            if (hashCode < 0)
            {
                continue;
            }

            entries.Add(new
            {
                key = ReadValueTypeFieldByName(entry, "key", depth),
                value = ReadValueTypeFieldByName(entry, "value", depth),
            });
        }

        return new
        {
            kind = "dictionary",
            count,
            returned = entries.Count,
            truncated = count > entries.Count,
            entries,
        };
    }

    /// <summary>
    /// ConcurrentDictionary&lt;TKey,TValue&gt; -- layout interne verifie par
    /// attache ClrMD reelle sur KillEngineClrTestTarget (script jetable, pas
    /// devine) : buffer prive `_tables` (type imbrique `Tables`), qui porte
    /// lui-meme `_buckets` (`VolatileNode[]` -- struct wrapper autour d'une
    /// seule reference `_node` vers un `Node` ou `null`, PAS un tableau de
    /// `Node` directement) et `_countPerLock` (`int[]`, un compteur par
    /// plage de verrous -- la somme donne le compte REEL vivant, la longueur
    /// physique de `_buckets` n'a aucun rapport avec le nombre d'elements).
    /// Chaque `Node` est une CLASSE (pas un struct comme
    /// `Dictionary&lt;K,V&gt;.Entry`) et porte `_key`/`_value`/`_next`/
    /// `_hashcode` ; les noeuds d'un meme bucket forment une chaine
    /// simplement liee terminee par `_next == null`. Contrairement a
    /// `HashSet&lt;T&gt;`, il n'y a pas de free-list a filtrer : `TryRemove`
    /// retire directement le noeud de sa chaine plutot que de le marquer
    /// supprime en place -- confirme concretement avec le graphe de test
    /// (`Inventory.ConcurrentCounters`, entree "stale" ajoutee puis retiree
    /// via `TryRemove`, "hits" mise a jour via `TryUpdate` pour verifier
    /// qu'un remplacement de noeud n'introduit pas de doublon dans la
    /// chaine).
    /// </summary>
    private static object DescribeConcurrentDictionary(ClrObject obj, int depth)
    {
        ClrObject tablesObj = obj.ReadObjectField("_tables");
        if (tablesObj.IsNull)
        {
            return new { kind = "concurrent_dictionary", count = 0, returned = 0, entries = Array.Empty<object?>() };
        }

        int count = 0;
        ClrObject countPerLockObj = tablesObj.ReadObjectField("_countPerLock");
        if (!countPerLockObj.IsNull && countPerLockObj.Type is { IsArray: true })
        {
            ClrArray countArray = countPerLockObj.AsArray();
            for (int i = 0; i < countArray.GetLength(0); ++i)
            {
                count += Math.Max(0, countArray.GetValue<int>(i));
            }
        }

        var entries = new List<object?>();
        ClrObject bucketsObj = tablesObj.ReadObjectField("_buckets");
        if (!bucketsObj.IsNull && bucketsObj.Type is { IsArray: true })
        {
            ClrArray bucketsArray = bucketsObj.AsArray();
            int bucketCount = bucketsArray.GetLength(0);
            for (int i = 0; i < bucketCount && entries.Count < MaxCollectionItems; ++i)
            {
                ClrValueType volatileNode = bucketsArray.GetStructValue(i);
                ClrInstanceField? nodeField = volatileNode.Type?.GetFieldByName("_node");
                if (nodeField is null) continue;
                ClrObject node = volatileNode.ReadObjectField(nodeField);

                // Borne de securite contre une chaine anormalement longue ou
                // corrompue -- une chaine saine ne cycle jamais (chainage
                // simple, jamais circulaire ici contrairement a LinkedList<T>).
                int guard = 0;
                while (!node.IsNull && entries.Count < MaxCollectionItems && guard++ < MaxCollectionItems)
                {
                    ClrInstanceField? keyField = node.Type?.GetFieldByName("_key");
                    ClrInstanceField? valueField = node.Type?.GetFieldByName("_value");
                    entries.Add(new
                    {
                        key = keyField is null ? null : ReadFieldValue(node, keyField, depth),
                        value = valueField is null ? null : ReadFieldValue(node, valueField, depth),
                    });
                    node = node.ReadObjectField("_next");
                }
            }
        }

        return new
        {
            kind = "concurrent_dictionary",
            count,
            returned = entries.Count,
            truncated = count > entries.Count,
            entries,
        };
    }

    /// <summary>
    /// HashSet&lt;T&gt; -- layout interne verifie par attache ClrMD reelle sur
    /// KillEngineClrTestTarget (script jetable, pas devine) : buffer prive
    /// `_entries` (`HashSet&lt;T&gt;+Entry[]`, champs `HashCode`/`Next`/`Value`,
    /// PAS `hashCode`/`next`/`value` en minuscule comme `Dictionary&lt;K,V&gt;.Entry`
    /// -- deux structures internes distinctes malgre la ressemblance de nom)
    /// + compteurs `_count`/`_freeCount`.
    ///
    /// **Piege reel rencontre pendant la verification** (documente plutot que
    /// silencie) : une premiere version de cette methode reutilisait par
    /// analogie l'heuristique de <see cref="DescribeDictionary"/> ("HashCode
    /// negatif == entree supprimee"), correcte pour `Dictionary&lt;K,V&gt;`
    /// (son `hashCode` est masque a une valeur TOUJOURS non-negative quand
    /// l'entree est vivante, `&amp; 0x7FFFFFFF`). Une attache ClrMD reelle sur
    /// `Inventory.Tags` avec un `Remove()` volontaire (`ObjectGraph.cs`) a
    /// revele que `HashSet&lt;T&gt;.Entry.HashCode` n'est PAS masque : des
    /// entrees bien VIVANTES ("common", "starter", "verified") ont un
    /// `HashCode` negatif tout a fait normal, ce qui faisait disparaitre a
    /// tort la totalite du HashSet. Le marqueur fiable (identique a
    /// `HashSet&lt;T&gt;.Enumerator.MoveNext()` cote BCL) est le champ `Next` :
    /// `&gt;= -1` pour une entree vivante (fin de chaine de bucket ou
    /// chainage normal), `&lt;= -2` pour une entree du free-list (encodage
    /// `StartOfFreeList(-3) - prochainIndexLibre`). L'iteration est aussi
    /// bornee a `_count` slots (pas la longueur physique du tableau, qui peut
    /// contenir des slots jamais initialises au-dela) -- le nombre
    /// d'elements REELLEMENT vivants est `_count - _freeCount` (litteralement
    /// `HashSet&lt;T&gt;.Count` cote BCL).
    /// </summary>
    private static object DescribeHashSet(ClrObject obj, int depth)
    {
        ClrObject entriesObject = obj.ReadObjectField("_entries");
        if (entriesObject.IsNull || entriesObject.Type is null || !entriesObject.Type.IsArray)
        {
            return new { kind = "hashset", count = 0, returned = 0, items = Array.Empty<object?>() };
        }

        int usedSlots = Math.Max(0, SafeReadIntField(obj, "_count"));
        int freeCount = Math.Max(0, SafeReadIntField(obj, "_freeCount"));
        int liveCount = Math.Max(0, usedSlots - freeCount);
        ClrArray entriesArray = entriesObject.AsArray();
        int physicalLength = entriesArray.GetLength(0);
        int slotsToScan = Math.Min(usedSlots, physicalLength);
        ClrType? entryType = entriesObject.Type.ComponentType;
        ClrInstanceField? valueField = entryType?.GetFieldByName("Value");

        var items = new List<object?>();
        for (int i = 0; i < slotsToScan && items.Count < MaxCollectionItems; ++i)
        {
            ClrValueType entry = entriesArray.GetStructValue(i);
            int next = TryReadValueTypeIntField(entry, "Next", out int n) ? n : -1;
            if (next < -1)
            {
                continue;
            }
            items.Add(valueField is null ? null : ReadValueTypeFieldValue(entry, valueField, depth));
        }

        return new
        {
            kind = "hashset",
            elementType = valueField?.Type?.Name,
            count = liveCount,
            returned = items.Count,
            truncated = liveCount > items.Count,
            items,
        };
    }

    /// <summary>
    /// Queue&lt;T&gt; -- layout interne verifie par attache ClrMD reelle : buffer
    /// prive `_array` + `_head`/`_tail`/`_size` (buffer CIRCULAIRE, pas un
    /// simple tableau 0.._size comme List&lt;T&gt;). L'element logique i est a
    /// l'index physique `(_head + i) % capacite` -- verifie concretement avec
    /// une sequence Enqueue/Dequeue/Enqueue qui force `_head > 0` ET un
    /// retour a zero de `_tail` (voir ObjectGraph.cs, `Inventory.ItemQueue`,
    /// et docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour le detail de la verification).
    /// </summary>
    private static object DescribeQueue(ClrObject obj, int depth)
    {
        ClrObject arrayObject = obj.ReadObjectField("_array");
        if (arrayObject.IsNull || arrayObject.Type is null || !arrayObject.Type.IsArray)
        {
            return new { kind = "queue", count = 0, returned = 0, items = Array.Empty<object?>() };
        }

        int head = SafeReadIntField(obj, "_head");
        int size = Math.Max(0, SafeReadIntField(obj, "_size"));
        ClrArray array = arrayObject.AsArray();
        int capacity = array.GetLength(0);
        int returned = Math.Max(0, Math.Min(size, MaxCollectionItems));
        ClrType? componentType = arrayObject.Type.ComponentType;

        var items = new List<object?>(returned);
        for (int i = 0; i < returned; ++i)
        {
            int physicalIndex = capacity == 0 ? 0 : (head + i) % capacity;
            items.Add(ReadArrayElement(array, componentType, physicalIndex, depth));
        }

        return new
        {
            kind = "queue",
            elementType = componentType?.Name,
            count = size,
            returned = items.Count,
            truncated = size > items.Count,
            items,
        };
    }

    /// <summary>
    /// Stack&lt;T&gt; -- layout interne verifie par attache ClrMD reelle : buffer
    /// prive `_array` + `_size`, PAS de champ de tete circulaire (contrairement
    /// a Queue&lt;T&gt;) -- les elements 0.._size-1 sont dans l'ordre d'empilement
    /// (index 0 = premier empile / le plus ancien). Restitue en ordre "sommet
    /// d'abord" (items[0] = ce que Pop() renverrait), pour matcher
    /// Stack&lt;T&gt;.GetEnumerator() cote BCL -- verifie avec une sequence
    /// Push/Pop/Push (voir ObjectGraph.cs, `Inventory.ItemStack`).
    /// </summary>
    private static object DescribeStack(ClrObject obj, int depth)
    {
        ClrObject arrayObject = obj.ReadObjectField("_array");
        if (arrayObject.IsNull || arrayObject.Type is null || !arrayObject.Type.IsArray)
        {
            return new { kind = "stack", count = 0, returned = 0, items = Array.Empty<object?>() };
        }

        int size = Math.Max(0, SafeReadIntField(obj, "_size"));
        ClrArray array = arrayObject.AsArray();
        int returned = Math.Max(0, Math.Min(size, MaxCollectionItems));
        ClrType? componentType = arrayObject.Type.ComponentType;

        var items = new List<object?>(returned);
        for (int i = 0; i < returned; ++i)
        {
            int physicalIndex = size - 1 - i;
            items.Add(ReadArrayElement(array, componentType, physicalIndex, depth));
        }

        return new
        {
            kind = "stack",
            elementType = componentType?.Name,
            count = size,
            returned = items.Count,
            truncated = size > items.Count,
            items,
        };
    }

    /// <summary>
    /// LinkedList&lt;T&gt; -- layout interne verifie par attache ClrMD reelle sur
    /// KillEngineClrTestTarget (script jetable dans le scratchpad de session,
    /// pas devine) : PAS de buffer tableau -- chaine de noeuds
    /// `LinkedListNode&lt;T&gt;` (classe, pas struct) relies par les champs
    /// d'instance `next`/`prev` (minuscule), chaque noeud portant sa valeur
    /// dans le champ `item` (minuscule) et un lien `list` vers la
    /// `LinkedList&lt;T&gt;` proprietaire. La liste elle-meme expose `head`
    /// (le PREMIER noeud logique, ou null si vide) et `count`.
    ///
    /// **Confirme CIRCULAIRE en interne** (verifie concretement avec une
    /// sequence AddFirst/AddLast/Remove qui produit 3 noeuds vivants, voir
    /// `Inventory.LinkedTags`/`ObjectGraph.cs`) : en partant de `head` et en
    /// suivant `next` a repetition, le noeud `count`-ieme (dernier logique)
    /// a bien `next` qui pointe DE NOUVEAU vers `head` plutot que vers
    /// `null` -- la sortie de boucle doit donc se faire par comptage
    /// (`count` noeuds parcourus) et/ou en detectant le retour a l'adresse
    /// de `head`, jamais en attendant un `next` null qui n'arrive pas.
    /// `prev` n'est pas utilise ici (parcours avant uniquement).
    /// </summary>
    private static object DescribeLinkedList(ClrObject obj, int depth)
    {
        int count = Math.Max(0, SafeReadIntField(obj, "count"));
        ClrObject headObj = obj.ReadObjectField("head");
        var items = new List<object?>();
        string? elementType = null;

        if (!headObj.IsNull)
        {
            ulong headAddress = headObj.Address;
            ClrObject current = headObj;
            int steps = Math.Min(count, MaxCollectionItems);

            for (int i = 0; i < steps; ++i)
            {
                ClrInstanceField? itemField = current.Type?.GetFieldByName("item");
                elementType ??= itemField?.Type?.Name;
                items.Add(itemField is null ? null : ReadFieldValue(current, itemField, depth));

                ClrObject next = current.ReadObjectField("next");
                if (next.IsNull || next.Address == headAddress)
                {
                    // Fin de la chaine circulaire (retour a head) ou noeud
                    // orphelin inattendu -- s'arrete proprement plutot que de
                    // boucler.
                    break;
                }
                current = next;
            }
        }

        return new
        {
            kind = "linkedlist",
            elementType,
            count,
            returned = items.Count,
            truncated = count > items.Count,
            items,
        };
    }

    /// <summary>
    /// Parcours EN ORDRE (in-order : gauche, noeud, droite) iteratif d'un
    /// arbre rouge-noir interne partage par `SortedSet&lt;T&gt;` et le
    /// `TreeSet&lt;T&gt;` prive de `SortedDictionary&lt;K,V&gt;` -- verifie par
    /// attache ClrMD reelle : les deux s'appuient sur la MEME classe de noeud
    /// generique `SortedSet&lt;T&gt;+Node` (proprietes auto-implementees, donc
    /// champs backing `&lt;Left&gt;k__BackingField`/`&lt;Right&gt;k__BackingField`/
    /// `&lt;Item&gt;k__BackingField`/`&lt;Color&gt;k__BackingField` -- `Color` n'est
    /// pas lu ici, non necessaire pour restituer le contenu). Un parcours
    /// in-order d'un arbre binaire de recherche produit naturellement les
    /// elements TRIES, sans reimplementer de logique de comparaison --
    /// exactement ce que `SortedSet&lt;T&gt;.GetEnumerator()`/
    /// `SortedDictionary&lt;K,V&gt;.GetEnumerator()` font cote BCL.
    ///
    /// Implementation ITERATIVE (pile explicite, pas de recursion) : s'arrete
    /// des que <paramref name="limit"/> noeuds ont ete produits, borne
    /// additionnellement par <see cref="MaxSortedTreeNodesVisited"/> comme
    /// garde-fou contre une structure corrompue/degenerescente (un arbre
    /// rouge-noir bien forme a une hauteur O(log n), donc cette seconde borne
    /// ne devrait jamais etre le facteur limitant en usage normal).
    /// </summary>
    private static List<ClrObject> WalkSortedTreeNodesInOrder(ClrObject root, int limit)
    {
        var result = new List<ClrObject>(Math.Max(0, Math.Min(limit, MaxCollectionItems)));
        if (root.IsNull || limit <= 0)
        {
            return result;
        }

        var stack = new Stack<ClrObject>();
        ClrObject current = root;
        int nodesVisited = 0;

        while ((!current.IsNull || stack.Count > 0) && result.Count < limit)
        {
            while (!current.IsNull)
            {
                if (nodesVisited >= MaxSortedTreeNodesVisited)
                {
                    return result;
                }
                nodesVisited++;
                stack.Push(current);
                current = current.ReadObjectField("<Left>k__BackingField");
            }

            current = stack.Pop();
            result.Add(current);
            current = current.ReadObjectField("<Right>k__BackingField");
        }

        return result;
    }

    /// <summary>
    /// SortedDictionary&lt;K,V&gt; -- layout interne verifie par attache ClrMD
    /// reelle : pas de buffer tableau, delegue entierement a un champ prive
    /// `_set` de type `TreeSet&lt;KeyValuePair&lt;K,V&gt;&gt;` (arbre rouge-noir),
    /// lui-meme expose son compte via `count` et sa racine via `root`. Chaque
    /// noeud de l'arbre porte la paire cle/valeur ENTIERE dans son champ
    /// `&lt;Item&gt;k__BackingField` (un `KeyValuePair&lt;K,V&gt;`, struct) --
    /// deballe via les memes noms de champs internes `key`/`value` (minuscule)
    /// que `Dictionary&lt;K,V&gt;.Entry` (verifie identique par attache reelle).
    /// Parcours in-order via <see cref="WalkSortedTreeNodesInOrder"/> --
    /// restitue les entrees TRIEES PAR CLE (verifie concretement avec des
    /// insertions volontairement en DESORDRE, voir
    /// `Inventory.SortedCurrencies`/`ObjectGraph.cs`).
    /// </summary>
    private static object DescribeSortedDictionary(ClrObject obj, int depth)
    {
        ClrObject setObj = obj.ReadObjectField("_set");
        if (setObj.IsNull)
        {
            return new { kind = "sorted_dictionary", count = 0, returned = 0, entries = Array.Empty<object?>() };
        }

        int count = Math.Max(0, SafeReadIntField(setObj, "count"));
        ClrObject rootObj = setObj.ReadObjectField("root");
        List<ClrObject> nodes = WalkSortedTreeNodesInOrder(rootObj, Math.Min(count, MaxCollectionItems));

        var entries = new List<object?>(nodes.Count);
        foreach (ClrObject node in nodes)
        {
            ClrInstanceField? itemField = node.Type?.GetFieldByName("<Item>k__BackingField");
            if (itemField is null || itemField.Type?.IsValueType != true)
            {
                entries.Add(null);
                continue;
            }
            ClrValueType kvp = node.ReadValueTypeField(itemField.Name!);
            entries.Add(new
            {
                key = ReadValueTypeFieldByName(kvp, "key", depth),
                value = ReadValueTypeFieldByName(kvp, "value", depth),
            });
        }

        return new
        {
            kind = "sorted_dictionary",
            count,
            returned = entries.Count,
            truncated = count > entries.Count,
            entries,
        };
    }

    /// <summary>
    /// SortedSet&lt;T&gt; -- meme arbre rouge-noir que le `TreeSet` interne de
    /// `SortedDictionary&lt;K,V&gt;` ci-dessus (racine directement sur le champ
    /// `root` de l'objet, pas besoin de descendre via un champ `_set`
    /// intermediaire). Chaque noeud porte sa valeur DIRECTEMENT dans
    /// `&lt;Item&gt;k__BackingField` (pas une paire cle/valeur). Parcours in-order
    /// via <see cref="WalkSortedTreeNodesInOrder"/> -- restitue les elements
    /// TRIES (verifie concretement avec des insertions volontairement en
    /// DESORDRE, voir `Inventory.SortedScores`/`ObjectGraph.cs`).
    /// </summary>
    private static object DescribeSortedSet(ClrObject obj, int depth)
    {
        int count = Math.Max(0, SafeReadIntField(obj, "count"));
        ClrObject rootObj = obj.ReadObjectField("root");
        List<ClrObject> nodes = WalkSortedTreeNodesInOrder(rootObj, Math.Min(count, MaxCollectionItems));

        ClrInstanceField? itemField = null;
        var items = new List<object?>(nodes.Count);
        foreach (ClrObject node in nodes)
        {
            itemField ??= node.Type?.GetFieldByName("<Item>k__BackingField");
            items.Add(itemField is null ? null : ReadFieldValue(node, itemField, depth));
        }

        return new
        {
            kind = "sorted_set",
            elementType = itemField?.Type?.Name,
            count,
            returned = items.Count,
            truncated = count > items.Count,
            items,
        };
    }

    /// <summary>
    /// Tableau multidimensionnel (ex: int[,], Rank > 1) -- API confirmee par
    /// attache ClrMD reelle : <see cref="ClrArray.Rank"/>, <see
    /// cref="ClrArray.GetLength"/>, et les surcharges indexees par
    /// <c>int[]</c> (<see cref="ClrArray.GetValue{T}(int[])"/>, <c>GetObjectValue(int[])</c>,
    /// <c>GetStructValue(int[])</c>). Restitue en liste PLATE (ordre "row-major",
    /// dernier indice varie le plus vite) plutot qu'en tableau de tableaux --
    /// plus simple a borner uniformement par <see cref="MaxCollectionItems"/>
    /// sur le nombre total d'elements plutot que par dimension ; les
    /// dimensions sont exposees separement pour reconstruire la structure
    /// cote appelant si besoin.
    /// </summary>
    private static object DescribeMultiDimensionalArray(ClrObject arrayObject, int depth)
    {
        ClrArray array = arrayObject.AsArray();
        int rank = array.Rank;
        var dimensions = new int[rank];
        long total = 1;
        for (int d = 0; d < rank; ++d)
        {
            dimensions[d] = array.GetLength(d);
            total *= dimensions[d];
        }

        ClrType? componentType = arrayObject.Type?.ComponentType;
        int returned = (int)Math.Max(0, Math.Min(total, MaxCollectionItems));
        var items = new List<object?>(returned);
        var indices = new int[rank];
        for (int flat = 0; flat < returned; ++flat)
        {
            int remaining = flat;
            for (int d = rank - 1; d >= 0; --d)
            {
                indices[d] = dimensions[d] == 0 ? 0 : remaining % dimensions[d];
                remaining /= Math.Max(1, dimensions[d]);
            }
            items.Add(ReadMultiDimensionalElement(array, componentType, indices, depth));
        }

        return new
        {
            kind = "multidim_array",
            rank,
            dimensions,
            elementType = componentType?.Name,
            count = total,
            returned = items.Count,
            truncated = total > items.Count,
            indexOrder = "row-major (flat = ((i0 * dim1 + i1) * dim2 + i2) ... ; dernier indice varie le plus vite)",
            items,
        };
    }

    private static object? ReadMultiDimensionalElement(ClrArray array, ClrType? componentType, int[] indices, int depth)
    {
        if (componentType is null)
        {
            return null;
        }

        if (componentType.ElementType == ClrElementType.String)
        {
            ClrObject str = array.GetObjectValue(indices);
            return str.IsNull ? null : str.AsString(4096);
        }

        if (IsPrimitiveElement(componentType.ElementType))
        {
            return ReadMultiDimensionalPrimitive(array, componentType.ElementType, indices);
        }

        if (!componentType.IsValueType)
        {
            ClrObject refObj = array.GetObjectValue(indices);
            return refObj.IsNull ? null : DescribeObjectReference(refObj, depth);
        }

        ClrValueType value = array.GetStructValue(indices);
        return DescribeValueType(value, depth);
    }

    private static object ReadMultiDimensionalPrimitive(ClrArray array, ClrElementType elementType, int[] indices)
    {
        return elementType switch
        {
            ClrElementType.Boolean => array.GetValue<bool>(indices),
            ClrElementType.Char => (int)array.GetValue<char>(indices),
            ClrElementType.Int8 => array.GetValue<sbyte>(indices),
            ClrElementType.UInt8 => array.GetValue<byte>(indices),
            ClrElementType.Int16 => array.GetValue<short>(indices),
            ClrElementType.UInt16 => array.GetValue<ushort>(indices),
            ClrElementType.Int32 => array.GetValue<int>(indices),
            ClrElementType.UInt32 => array.GetValue<uint>(indices),
            ClrElementType.Int64 => array.GetValue<long>(indices),
            ClrElementType.UInt64 => array.GetValue<ulong>(indices),
            ClrElementType.Float => array.GetValue<float>(indices),
            ClrElementType.Double => array.GetValue<double>(indices),
            _ => $"<primitif non gere: {elementType}>",
        };
    }

    private static object DescribeValueType(ClrValueType value, int depth)
    {
        var fields = new Dictionary<string, object?>();
        if (value.Type is not null)
        {
            foreach (ClrInstanceField field in value.Type.Fields)
            {
                if (field.Name is null) continue;
                fields[field.Name] = ReadValueTypeFieldValue(value, field, depth);
            }
        }

        return new
        {
            typeName = value.Type?.Name,
            fields,
        };
    }

    private static object? ReadValueTypeFieldByName(ClrValueType value, string fieldName, int depth)
    {
        ClrInstanceField? field = value.Type?.GetFieldByName(fieldName);
        return field is null ? null : ReadValueTypeFieldValue(value, field, depth);
    }

    private static object? ReadValueTypeFieldValue(ClrValueType value, ClrInstanceField field, int depth)
    {
        if (field.Name is null) return null;
        if (field.ElementType == ClrElementType.String)
        {
            return value.ReadStringField(field, 4096);
        }
        if (field.IsPrimitive)
        {
            return ReadValueTypePrimitive(value, field);
        }
        if (field.IsObjectReference)
        {
            ClrObject refObj = value.ReadObjectField(field);
            if (!refObj.IsNull && refObj.Type?.IsString == true)
            {
                return refObj.AsString(4096);
            }
            return refObj.IsNull ? null : DescribeObjectReference(refObj, depth);
        }

        // Struct imbriquee DANS un struct (ex: PlayerStats.HomeZone de type
        // Zone, qui contient elle-meme Coordinates) -- c'etait le placeholder
        // texte fixe avant le chantier "resolution recursive des structs
        // imbriques" (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md). Deballage
        // recursif borne par MaxValueTypeDepth : au-dela, placeholder
        // explicite plutot que de continuer (protection contre un type
        // generique imbrique de facon degeneree, pas une vraie boucle -- les
        // structs .NET ne peuvent pas se contenir eux-memes par valeur).
        if (depth < MaxValueTypeDepth)
        {
            ClrValueType nested = value.ReadValueTypeField(field);
            return DescribeValueType(nested, depth + 1);
        }

        return $"<profondeur maximale de struct imbriquee atteinte ({MaxValueTypeDepth}): {field.Type?.Name ?? field.ElementType.ToString()}>";
    }

    private static bool IsPrimitiveElement(ClrElementType elementType)
    {
        return elementType is ClrElementType.Boolean
            or ClrElementType.Char
            or ClrElementType.Int8
            or ClrElementType.UInt8
            or ClrElementType.Int16
            or ClrElementType.UInt16
            or ClrElementType.Int32
            or ClrElementType.UInt32
            or ClrElementType.Int64
            or ClrElementType.UInt64
            or ClrElementType.Float
            or ClrElementType.Double;
    }

    private static object ReadArrayPrimitive(ClrArray array, ClrElementType elementType, int index)
    {
        return elementType switch
        {
            ClrElementType.Boolean => array.GetValue<bool>(index),
            ClrElementType.Char => (int)array.GetValue<char>(index),
            ClrElementType.Int8 => array.GetValue<sbyte>(index),
            ClrElementType.UInt8 => array.GetValue<byte>(index),
            ClrElementType.Int16 => array.GetValue<short>(index),
            ClrElementType.UInt16 => array.GetValue<ushort>(index),
            ClrElementType.Int32 => array.GetValue<int>(index),
            ClrElementType.UInt32 => array.GetValue<uint>(index),
            ClrElementType.Int64 => array.GetValue<long>(index),
            ClrElementType.UInt64 => array.GetValue<ulong>(index),
            ClrElementType.Float => array.GetValue<float>(index),
            ClrElementType.Double => array.GetValue<double>(index),
            _ => $"<primitif non gere: {elementType}>",
        };
    }

    private static object ReadValueTypePrimitive(ClrValueType value, ClrInstanceField field)
    {
        return field.ElementType switch
        {
            ClrElementType.Boolean => value.ReadField<bool>(field),
            ClrElementType.Char => (int)value.ReadField<char>(field),
            ClrElementType.Int8 => value.ReadField<sbyte>(field),
            ClrElementType.UInt8 => value.ReadField<byte>(field),
            ClrElementType.Int16 => value.ReadField<short>(field),
            ClrElementType.UInt16 => value.ReadField<ushort>(field),
            ClrElementType.Int32 => value.ReadField<int>(field),
            ClrElementType.UInt32 => value.ReadField<uint>(field),
            ClrElementType.Int64 => value.ReadField<long>(field),
            ClrElementType.UInt64 => value.ReadField<ulong>(field),
            ClrElementType.Float => value.ReadField<float>(field),
            ClrElementType.Double => value.ReadField<double>(field),
            _ => $"<primitif non gere: {field.ElementType}>",
        };
    }

    private static int SafeReadIntField(ClrObject obj, string fieldName)
    {
        return obj.TryReadField<int>(fieldName, out int value) ? value : 0;
    }

    private static bool TryReadValueTypeIntField(ClrValueType value, string fieldName, out int result)
    {
        result = 0;
        ClrInstanceField? field = value.Type?.GetFieldByName(fieldName);
        if (field is null || field.ElementType != ClrElementType.Int32)
        {
            return false;
        }
        result = value.ReadField<int>(field);
        return true;
    }

    public object EnumerateRoots(string? typeSubstring)
    {
        var runtime = RequireRuntime();
        var heap = runtime.Heap;

        var results = new List<object>();
        foreach (ClrRoot root in heap.EnumerateRoots())
        {
            ClrObject rootObj = root.Object;
            if (rootObj.IsNull) continue;

            string? typeName = rootObj.Type?.Name;
            if (typeSubstring is not null && (typeName is null || !typeName.Contains(typeSubstring, StringComparison.Ordinal)))
            {
                continue;
            }

            results.Add(new
            {
                rootAddress = ToHex(root.Address),
                rootKind = root.RootKind.ToString(),
                isPinned = root.IsPinned,
                objectAddress = ToHex(rootObj.Address),
                objectTypeName = typeName,
            });
        }
        return results;
    }

    /// <summary>
    /// Chantier "investigation du root StaticVar manquant" (docs/
    /// KILLENGINE_CLR_INSPECTOR_SPEC.md) : <see cref="EnumerateRoots"/>
    /// (heap.EnumerateRoots) ne rapporte JAMAIS de root de type StaticVar sur
    /// ce runtime/cette version de ClrMD, meme avec une attache invasive
    /// (`suspend:true`) ou l'option `DataTargetOptions.ForceCompleteRuntimeEnumeration`
    /// -- verifie par attache reelle avant d'ecrire cette methode (script
    /// jetable dans le scratchpad de session), pas suppose. En revanche,
    /// ClrMD expose un mecanisme COMPLETEMENT DIFFERENT et fiable pour lire
    /// un champ static directement, sans dependre de l'enumeration des roots
    /// et sans attache invasive : <see cref="ClrType.StaticFields"/> +
    /// <see cref="ClrStaticField.ReadObject"/>/<c>Read&lt;T&gt;</c>/<c>ReadString</c>
    /// par <see cref="ClrAppDomain"/>. Cette methode resout donc le besoin
    /// pratique ("retrouver l'objet reference par un champ static connu")
    /// par un chemin different de celui envisage au depart (pas un nouveau
    /// mode d'attache "suspended", une API de lecture directe qui fonctionne
    /// deja en attache passive). Les types sont retrouves en parcourant
    /// `ClrModule.EnumerateTypeDefToMethodTableMap()` de chaque module charge
    /// (necessaire car un type purement static comme `TestRoot`, jamais
    /// instancie, n'apparait pas dans `heap.EnumerateObjects()`).
    /// </summary>
    public object FindStaticFields(string typeSubstring, string? fieldNameSubstring)
    {
        var runtime = RequireRuntime();
        string typeFilter = typeSubstring.Trim();
        if (typeFilter.Length == 0)
        {
            throw new ClrSessionException("Type requis pour enumerer des champs static CLR.");
        }
        string? fieldFilter = string.IsNullOrWhiteSpace(fieldNameSubstring) ? null : fieldNameSubstring.Trim();

        var results = new List<object>();
        var seenMethodTables = new HashSet<ulong>();
        int typesScanned = 0;

        foreach (ClrModule module in runtime.EnumerateModules())
        {
            foreach ((ulong methodTable, int _) in module.EnumerateTypeDefToMethodTableMap())
            {
                if (!seenMethodTables.Add(methodTable))
                {
                    continue;
                }

                ClrType? type = runtime.GetTypeByMethodTable(methodTable);
                if (type?.Name is null || type.StaticFields.Length == 0)
                {
                    continue;
                }
                if (!type.Name.Contains(typeFilter, StringComparison.Ordinal))
                {
                    continue;
                }
                typesScanned++;

                foreach (ClrStaticField field in type.StaticFields)
                {
                    if (fieldFilter is not null &&
                        (field.Name is null || !field.Name.Contains(fieldFilter, StringComparison.Ordinal)))
                    {
                        continue;
                    }

                    foreach (ClrAppDomain domain in runtime.AppDomains)
                    {
                        results.Add(DescribeStaticFieldValue(type, field, domain));
                        if (results.Count >= 200)
                        {
                            return BuildStaticFieldsResult(results, typeFilter, fieldFilter, typesScanned);
                        }
                    }
                }
            }
        }

        return BuildStaticFieldsResult(results, typeFilter, fieldFilter, typesScanned);
    }

    private static object BuildStaticFieldsResult(List<object> results, string typeFilter, string? fieldFilter, int typesScanned)
    {
        return new
        {
            success = results.Count > 0,
            typeSubstring = typeFilter,
            fieldNameSubstring = fieldFilter,
            typesScanned,
            matches = results,
        };
    }

    private static object DescribeStaticFieldValue(ClrType type, ClrStaticField field, ClrAppDomain domain)
    {
        object? value = null;
        string? objectAddress = null;
        string? objectTypeName = null;
        try
        {
            if (!field.IsInitialized(domain))
            {
                value = "<non initialise>";
            }
            else if (field.IsObjectReference)
            {
                ClrObject refObj = field.ReadObject(domain);
                if (!refObj.IsNull)
                {
                    objectAddress = ToHex(refObj.Address);
                    objectTypeName = refObj.Type?.Name;
                }
                value = objectAddress;
            }
            else if (field.ElementType == ClrElementType.String)
            {
                value = field.ReadString(domain);
            }
            else if (field.IsPrimitive)
            {
                value = ReadStaticPrimitive(field, domain);
            }
            else
            {
                value = $"<champ static value-type non deroule: {field.Type?.Name ?? field.ElementType.ToString()}>";
            }
        }
        catch (Exception ex)
        {
            value = $"<erreur lecture: {ex.Message}>";
        }

        return new
        {
            typeName = type.Name,
            fieldName = field.Name,
            elementType = field.ElementType.ToString(),
            appDomainId = domain.Id,
            value,
            objectAddress,
            objectTypeName,
        };
    }

    private static object ReadStaticPrimitive(ClrStaticField field, ClrAppDomain domain)
    {
        return field.ElementType switch
        {
            ClrElementType.Boolean => field.Read<bool>(domain),
            ClrElementType.Char => (int)field.Read<char>(domain),
            ClrElementType.Int8 => field.Read<sbyte>(domain),
            ClrElementType.UInt8 => field.Read<byte>(domain),
            ClrElementType.Int16 => field.Read<short>(domain),
            ClrElementType.UInt16 => field.Read<ushort>(domain),
            ClrElementType.Int32 => field.Read<int>(domain),
            ClrElementType.UInt32 => field.Read<uint>(domain),
            ClrElementType.Int64 => field.Read<long>(domain),
            ClrElementType.UInt64 => field.Read<ulong>(domain),
            ClrElementType.Float => field.Read<float>(domain),
            ClrElementType.Double => field.Read<double>(domain),
            _ => $"<primitif static non gere: {field.ElementType}>",
        };
    }

    // Chantier "GCRoot chain complet" (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) :
    // bornes par defaut/max pour FindGcRootPath. Point le plus exploratoire
    // de ce lot -- voir la doc pour l'honnetete sur ce qui marche vraiment
    // (un chemin trouve, pas garanti le plus court) vs les limites connues
    // (scan potentiellement lent sur un gros tas).
    private const int MaxGcRootPathDepth = 12;
    private const int MaxGcRootsScanned = 20_000;
    private const int MaxGcRootPathArrayElementsPerNode = 64;
    private const int MaxGcRootPathTotalNodesVisited = 200_000;
    private static readonly TimeSpan GcRootPathTimeBudget = TimeSpan.FromSeconds(15);

    /// <summary>
    /// Reconstruit le PLUS COURT chemin root -> ... -> objet cible a travers
    /// plusieurs sauts de references (equivalent approximatif de `!gcroot`
    /// SOS/WinDbg, pas une reimplementation exacte). Contrairement a la
    /// version initiale de ce chantier (BFS independant par root, premier
    /// chemin trouve pas garanti le plus court), cette version fait un seul
    /// BFS MULTI-SOURCE : tous les objets de <c>heap.EnumerateRoots()</c>
    /// (bornes a <paramref name="maxRootsScanned"/>) sont enfiles ENSEMBLE a
    /// la profondeur 0 dans UNE SEULE queue/UN SEUL ensemble <c>visited</c>
    /// partages (dedoublonnage : si plusieurs roots pointent vers le meme
    /// objet, il n'est enfile qu'une fois -- le premier root rencontre dans
    /// l'ordre d'enumeration "gagne" et sert de racine rapportee, choix
    /// deterministe et sans consequence puisque tous les roots de depart sont
    /// a egale profondeur 0). Un BFS explorant par profondeur croissante
    /// uniforme garantit que le PREMIER moment ou l'objet cible est atteint
    /// correspond au plus court chemin en nombre de sauts parmi TOUTES les
    /// sources combinees (propriete standard d'un BFS multi-source).
    ///
    /// Explore les champs de reference d'instance de chaque objet visite
    /// ainsi que les elements des tableaux/listes rencontres (bornes a
    /// <see cref="MaxGcRootPathArrayElementsPerNode"/> elements par noeud
    /// tableau -- reutilise la meme logique d'acces aux elements de tableau
    /// que <see cref="ReadArrayElement"/>/<see cref="DescribeArray"/>
    /// ailleurs dans ce fichier, juste sans deballage complet de la valeur).
    /// Les objets deja visites (par n'importe quelle source) sont marques
    /// (meme principe que la gestion de cycle existante pour `Player.Self`)
    /// pour eviter une boucle infinie ET pour ne jamais revisiter un noeud
    /// deja atteint par un chemin plus court ou de longueur egale.
    ///
    /// Bornes identiques a la version precedente, juste appliquees a une
    /// structure a source unique-mais-multiple au lieu d'une boucle externe
    /// par root : <paramref name="maxDepth"/>/<see cref="MaxGcRootPathDepth"/>
    /// (profondeur), <paramref name="maxRootsScanned"/>/<see
    /// cref="MaxGcRootsScanned"/> (nombre de sources initiales), <see
    /// cref="MaxGcRootPathArrayElementsPerNode"/> (elements de tableau par
    /// noeud), <see cref="MaxGcRootPathTotalNodesVisited"/> (compteur GLOBAL
    /// de noeuds visites -- deja global dans la version precedente, pas de
    /// changement de semantique ici) et <see cref="GcRootPathTimeBudget"/>
    /// (budget de temps global). Le travail total abattu est en fait
    /// STRICTEMENT INFERIEUR OU EGAL a la version precedente : chaque objet
    /// du tas n'est plus explore qu'une seule fois au total (au lieu
    /// potentiellement une fois par root qui l'atteint), donc pas de
    /// regression de performance sur un gros tas -- au contraire.
    /// </summary>
    public object FindGcRootPath(string targetAddressHex, int maxDepth, int maxRootsScanned)
    {
        var runtime = RequireRuntime();
        ulong targetAddress = ParseHexAddress(targetAddressHex);
        var heap = runtime.Heap;

        int depthLimit = Math.Clamp(maxDepth, 1, MaxGcRootPathDepth);
        int rootsLimit = Math.Clamp(maxRootsScanned, 1, MaxGcRootsScanned);

        var stopwatch = System.Diagnostics.Stopwatch.StartNew();
        int rootsScanned = 0;
        long nodesVisited = 0;
        bool budgetExceeded = false;

        // Un seul ensemble "visited" partage par TOUTES les sources -- coeur
        // de la garantie multi-source. Les noeuds racine (profondeur 0) ont
        // OriginRoot renseigne et ParentAddress null ; les autres noeuds
        // portent le lien vers leur parent dans le BFS pour permettre de
        // reconstruire le chemin par remontee une fois la cible trouvee.
        var visited = new Dictionary<ulong, GcRootPathVisitedNode>();
        var queue = new Queue<ClrObject>();

        foreach (ClrRoot root in heap.EnumerateRoots())
        {
            if (rootsScanned >= rootsLimit)
            {
                break;
            }
            rootsScanned++;

            ClrObject rootObj = root.Object;
            if (rootObj.IsNull || visited.ContainsKey(rootObj.Address))
            {
                // Dedoublonnage : un objet deja enfile par un root precedent
                // (a la meme profondeur 0) n'est pas re-enfile -- le premier
                // root rencontre reste celui rapporte pour cet objet.
                continue;
            }

            visited[rootObj.Address] = new GcRootPathVisitedNode(OriginRoot: root, ParentAddress: null, Kind: null, FieldName: null, Index: null, TypeName: rootObj.Type?.Name, Depth: 0);

            if (rootObj.Address == targetAddress)
            {
                return BuildFoundGcRootPathResultMultiSource(visited, targetAddress, rootsScanned, nodesVisited, stopwatch.Elapsed);
            }

            queue.Enqueue(rootObj);
        }

        while (queue.Count > 0)
        {
            if (stopwatch.Elapsed > GcRootPathTimeBudget || nodesVisited > MaxGcRootPathTotalNodesVisited)
            {
                budgetExceeded = true;
                break;
            }

            ClrObject current = queue.Dequeue();
            int currentDepth = visited[current.Address].Depth;
            if (currentDepth >= depthLimit)
            {
                continue;
            }

            foreach ((string kind, string? fieldName, int? index, ClrObject child) in
                     EnumerateGcRootPathReferences(current, MaxGcRootPathArrayElementsPerNode))
            {
                nodesVisited++;
                if (visited.ContainsKey(child.Address))
                {
                    // Deja atteint (par cette source ou une autre) a une
                    // profondeur <= celle-ci -- BFS garantit que le premier
                    // ajout est deja le plus court, rien a mettre a jour.
                    continue;
                }

                visited[child.Address] = new GcRootPathVisitedNode(OriginRoot: null, ParentAddress: current.Address, Kind: kind, FieldName: fieldName, Index: index, TypeName: child.Type?.Name, Depth: currentDepth + 1);

                if (child.Address == targetAddress)
                {
                    return BuildFoundGcRootPathResultMultiSource(visited, targetAddress, rootsScanned, nodesVisited, stopwatch.Elapsed);
                }

                queue.Enqueue(child);

                if (nodesVisited > MaxGcRootPathTotalNodesVisited)
                {
                    budgetExceeded = true;
                    break;
                }
            }

            if (budgetExceeded)
            {
                break;
            }
        }

        return new
        {
            success = false,
            targetAddress = ToHex(targetAddress),
            rootsScanned,
            nodesVisited,
            elapsedMs = stopwatch.ElapsedMilliseconds,
            budgetExceeded,
            message = budgetExceeded
                ? "Budget de temps/noeuds visites epuise avant de trouver un chemin -- augmenter maxDepth/maxRootsScanned ou reessayer (le resultat n'est pas une preuve d'absence de chemin, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md)."
                : "Aucun chemin trouve dans les limites exploree (rootsScanned/maxDepth) -- l'objet cible n'est peut-etre atteignable que via un type de collection ou une reference non parcourue par cette version bornee (WeakReference, ConditionalWeakTable, champs statiques intermediaires, etc.).",
        };
    }

    /// <summary>
    /// Noeud du BFS multi-source de <see cref="FindGcRootPath"/>. Un noeud
    /// racine (profondeur 0, direct dans <c>heap.EnumerateRoots()</c>) porte
    /// <see cref="OriginRoot"/> et un <see cref="ParentAddress"/> null ; tout
    /// autre noeud porte le lien vers son parent dans le BFS (adresse +
    /// nature de la reference qui y mene) pour permettre de reconstruire le
    /// chemin par remontee une fois la cible trouvee.
    /// </summary>
    private readonly record struct GcRootPathVisitedNode(ClrRoot? OriginRoot, ulong? ParentAddress, string? Kind, string? FieldName, int? Index, string? TypeName, int Depth);

    private static object BuildFoundGcRootPathResultMultiSource(Dictionary<ulong, GcRootPathVisitedNode> visited, ulong targetAddress, int rootsScanned, long nodesVisited, TimeSpan elapsed)
    {
        var reversedPath = new List<object>();
        ulong currentAddress = targetAddress;
        GcRootPathVisitedNode currentNode = visited[currentAddress];

        while (currentNode.ParentAddress is ulong parentAddress)
        {
            reversedPath.Add(new
            {
                kind = currentNode.Kind,
                fieldName = currentNode.FieldName,
                index = currentNode.Index,
                objectAddress = ToHex(currentAddress),
                typeName = currentNode.TypeName,
            });
            currentAddress = parentAddress;
            currentNode = visited[currentAddress];
        }
        reversedPath.Reverse();

        // A ce point, currentNode est un noeud racine (ParentAddress null),
        // donc OriginRoot est necessairement renseigne (invariant garanti a
        // la construction : seuls les noeuds racine ont ParentAddress null).
        // ClrRoot est une classe (pas une struct) dans ClrMD 4.0.732401,
        // verifie par reflexion avant d'ecrire ce code -- pas de `.Value`
        // ici, juste le null-forgiving operator sur la reference.
        ClrRoot root = currentNode.OriginRoot!;

        return new
        {
            success = true,
            targetAddress = ToHex(targetAddress),
            rootKind = root.RootKind.ToString(),
            rootAddress = ToHex(root.Address),
            rootObjectAddress = ToHex(root.Object.Address),
            rootObjectTypeName = root.Object.Type?.Name,
            depth = reversedPath.Count,
            path = reversedPath,
            rootsScanned,
            nodesVisited,
            elapsedMs = elapsed.TotalMilliseconds,
            shortestPathGuaranteed = true,
            note = "Plus court chemin garanti (BFS multi-source unique explorant simultanement tous les roots scannes -- voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md).",
        };
    }

    private static IEnumerable<(string Kind, string? FieldName, int? Index, ClrObject Child)> EnumerateGcRootPathReferences(ClrObject current, int maxArrayElementsPerNode)
    {
        ClrType? type = current.Type;
        if (type is null)
        {
            yield break;
        }

        if (type.IsArray)
        {
            ClrType? componentType = type.ComponentType;
            if (componentType is not null && !componentType.IsValueType && componentType.ElementType != ClrElementType.String)
            {
                ClrArray array = current.AsArray();
                int length = Math.Min(array.GetLength(0), maxArrayElementsPerNode);
                for (int i = 0; i < length; ++i)
                {
                    ClrObject element;
                    try
                    {
                        element = array.GetObjectValue(i);
                    }
                    catch
                    {
                        continue;
                    }
                    if (element.IsNull)
                    {
                        continue;
                    }
                    yield return ("index", null, i, element);
                }
            }
            yield break;
        }

        foreach (ClrInstanceField field in type.Fields)
        {
            if (!field.IsObjectReference || field.Name is null || field.ElementType == ClrElementType.String)
            {
                continue;
            }
            ClrObject child;
            try
            {
                child = current.ReadObjectField(field.Name);
            }
            catch
            {
                continue;
            }
            if (child.IsNull)
            {
                continue;
            }
            yield return ("field", field.Name, null, child);
        }
    }

    // Bornes du rapport d'objet -- volontairement plus faibles que celles de
    // FindGcRootPath : DescribeObject() fait un travail bien plus lourd par
    // noeud (deballage complet des champs + collections, jusqu'a
    // MaxCollectionItems chacune) que la simple enumeration de references, un
    // rapport sur un graphe large serait sinon soit tres lent, soit un
    // document illisible.
    private const int MaxObjectReportDepth = 6;
    private const int MaxObjectReportNodes = 300;
    private const int DefaultObjectReportDepth = 3;
    private const int DefaultObjectReportNodes = 50;
    private static readonly TimeSpan ObjectReportTimeBudget = TimeSpan.FromSeconds(20);

    /// <summary>
    /// Genere un rapport borne d'un objet et de son graphe atteignable :
    /// parcours en largeur (BFS) reutilisant EXACTEMENT
    /// <see cref="EnumerateGcRootPathReferences"/> (meme notion de "reference
    /// visitable" que le chemin GCRoot -- champs reference + elements de
    /// tableau de references, donc descend aussi dans le stockage interne de
    /// List&lt;T&gt;/Dictionary&lt;K,V&gt;/etc. via leurs champs prives,
    /// exactement comme les tests FindGcRootPath l'ont deja verifie). Chaque
    /// noeud visite est decrit par <see cref="DescribeObject"/> -- la MEME
    /// sortie que <see cref="ReadObject"/> pour un seul objet (champs,
    /// fieldDetails, collections deroulees), reutilisee telle quelle plutot
    /// que redupliquee. Inclut aussi, optionnellement, comment atteindre
    /// l'objet racine depuis un GC root en reutilisant
    /// <see cref="FindGcRootPath"/> tel quel (meme honnetete sur ses limites
    /// deja documentees : premier chemin trouve, pas garanti le plus court).
    ///
    /// Sortie structuree en JSON (liste plate de noeuds + aretes
    /// discoveredVia, pas un arbre JSON imbrique) : plus simple a serialiser
    /// et a parcourir cote appelant, et gere naturellement les cycles/
    /// references partagees (un noeud n'apparait qu'une fois, meme s'il est
    /// atteint par plusieurs chemins) sans dupliquer son contenu. Le rendu en
    /// texte lisible (indentation, hierarchie) est laisse a l'appelant --
    /// cote KillEngine, c'est `ClrInspectorView.vue` qui formate ce JSON en
    /// document texte pour l'utilisateur.
    /// </summary>
    public object GenerateObjectReport(string objectAddressHex, int maxDepth, int maxNodes, bool includeGcRootChain)
    {
        var runtime = RequireRuntime();
        ulong rootAddress = ParseHexAddress(objectAddressHex);
        ClrObject rootObj = runtime.Heap.GetObject(rootAddress);
        if (rootObj.IsNull || !rootObj.IsValid)
        {
            throw new ClrSessionException($"Adresse {objectAddressHex} : objet invalide ou null (probablement deplace/collecte -- relire avec findObjectsByType apres un flushCachedData).");
        }

        int depthLimit = maxDepth <= 0 ? DefaultObjectReportDepth : Math.Clamp(maxDepth, 1, MaxObjectReportDepth);
        int nodesLimit = maxNodes <= 0 ? DefaultObjectReportNodes : Math.Clamp(maxNodes, 1, MaxObjectReportNodes);

        var stopwatch = System.Diagnostics.Stopwatch.StartNew();
        var visited = new HashSet<ulong> { rootObj.Address };
        var queue = new Queue<(ClrObject Obj, ulong? ParentAddress, string? Kind, string? FieldName, int? Index, int Depth)>();
        queue.Enqueue((rootObj, null, null, null, null, 0));

        var nodes = new List<object>();
        bool truncatedByDepth = false;
        bool truncatedByNodes = false;
        bool truncatedByTime = false;

        while (queue.Count > 0)
        {
            if (nodes.Count >= nodesLimit)
            {
                truncatedByNodes = true;
                break;
            }
            if (stopwatch.Elapsed > ObjectReportTimeBudget)
            {
                truncatedByTime = true;
                break;
            }

            (ClrObject current, ulong? parentAddress, string? kind, string? fieldName, int? index, int depth) = queue.Dequeue();

            object described;
            try
            {
                described = DescribeObject(current);
            }
            catch (Exception ex)
            {
                described = new { address = ToHex(current.Address), typeName = current.Type?.Name, error = $"<erreur lecture: {ex.Message}>" };
            }

            nodes.Add(new
            {
                address = ToHex(current.Address),
                depth,
                discoveredVia = parentAddress is null ? null : new { parentAddress = ToHex(parentAddress.Value), kind, fieldName, index },
                node = described,
            });

            foreach ((string childKind, string? childFieldName, int? childIndex, ClrObject child) in
                     EnumerateGcRootPathReferences(current, MaxGcRootPathArrayElementsPerNode))
            {
                if (visited.Contains(child.Address))
                {
                    continue;
                }
                if (depth >= depthLimit)
                {
                    truncatedByDepth = true;
                    continue;
                }
                visited.Add(child.Address);
                queue.Enqueue((child, current.Address, childKind, childFieldName, childIndex, depth + 1));
            }
        }

        object? gcRootChain = null;
        if (includeGcRootChain)
        {
            try
            {
                gcRootChain = FindGcRootPath(objectAddressHex, MaxGcRootPathDepth, MaxGcRootsScanned);
            }
            catch (Exception ex)
            {
                gcRootChain = new { success = false, error = $"<erreur resolution GCRoot: {ex.Message}>" };
            }
        }

        return new
        {
            success = true,
            rootAddress = ToHex(rootObj.Address),
            rootTypeName = rootObj.Type?.Name,
            generatedAtUtc = DateTime.UtcNow.ToString("o"),
            nodeCount = nodes.Count,
            maxDepth = depthLimit,
            maxNodes = nodesLimit,
            truncated = truncatedByDepth || truncatedByNodes || truncatedByTime,
            truncatedByDepth,
            truncatedByNodes,
            truncatedByTime,
            elapsedMs = stopwatch.ElapsedMilliseconds,
            gcRootChain,
            nodes,
        };
    }

    // Types de parametre primitif supportes pour l'appel reel d'un setter
    // (v1 : bool + entiers 8/16/32/64 signes/non signes). Etendu en PHASE 59
    // (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) pour couvrir Single/Double :
    // la convention d'appel x64 Windows passe un 2e argument flottant en
    // XMM1, pas RDX -- ClrSession se contente toujours de RESOUDRE l'adresse
    // (elle ne sait rien de la convention d'appel), c'est
    // ApplicationController::buildCallInstanceMethodShellcode (cote natif)
    // qui charge XMM1 au lieu de RDX quand parameterType vaut "Single" ou
    // "Double" -- voir la section dediee dans le spec doc.
    // Noms exacts tels que rapportes par ClrMethod.Signature -- verifies par
    // attache ClrMD reelle sur ce process de test (pas devines), reflexion
    // ponctuelle avant d'ecrire cette methode : Boolean/SByte/Byte/Int16/
    // UInt16/Int32/UInt32/Int64/UInt64/Single/Double pour les primitifs
    // (noms courts CLR, sans namespace) ; un type non primitif (string,
    // objet, struct) apparait toujours prefixe de son namespace complet
    // (ex: "System.String", "KillEngine.ClrTestTarget.Item"), donc jamais en
    // collision avec cette liste.
    private static readonly HashSet<string> SupportedInstanceMethodParameterTypes = new(StringComparer.Ordinal)
    {
        "Boolean", "SByte", "Byte", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64", "Single", "Double",
    };

    /// <summary>
    /// Resout l'adresse native deja JITtee d'un setter d'instance reel (pas
    /// static, 0 ou 1 parametre) pour permettre a KillEngine (cote natif,
    /// injection shellcode) de l'appeler directement -- ClrMD est une API de
    /// lecture passive (DAC), elle n'execute jamais de code cible elle-meme,
    /// cette methode se limite donc a la RESOLUTION d'adresse. Le parametre,
    /// s'il y en a un, peut etre primitif (bool/int8..64/uint8..64/single/double),
    /// un type REFERENCE (classe/string/interface -- chantier "setters a
    /// parametre objet/string", <c>parameterIsReferenceType: true</c> dans le
    /// resultat) pointant vers un objet DEJA EXISTANT sur le tas, OU un type
    /// VALEUR (struct) dont TOUS les champs sont primitifs et dont la taille
    /// totale est 1/2/4/8 octets (<c>parameterIsStruct: true</c>, chantier
    /// "setters a parametre struct") -- un struct plus grand ou contenant un
    /// champ non primitif reste rejete explicitement (voir <see
    /// cref="ParseInstanceMethodParameters"/> et la resolution
    /// IsValueType ci-dessous). Voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md
    /// pour le detail complet du perimetre et son mecanisme d'appel
    /// (shellcode x64 cote ApplicationController::callClrInstanceMethod).
    /// </summary>
    public object ResolveInstanceMethodAddress(string objectAddressHex, string methodName)
    {
        var runtime = RequireRuntime();
        ulong objectAddress = ParseHexAddress(objectAddressHex);

        ClrObject obj = runtime.Heap.GetObject(objectAddress);
        if (obj.IsNull || !obj.IsValid || obj.Type is null)
        {
            throw new ClrSessionException($"Adresse {objectAddressHex} : objet invalide ou null avant resolution de methode.");
        }

        string requestedName = methodName.Trim();
        if (requestedName.Length == 0)
        {
            throw new ClrSessionException("Nom de methode CLR vide.");
        }

        // Nom exact d'abord ; si introuvable et que l'appelant n'a pas deja
        // prefixe "set_", retente avec le prefixe -- permet de passer
        // directement le nom de propriete C# ("Health") plutot que le nom
        // de methode compile ("set_Health").
        ClrMethod? method = obj.Type.Methods.FirstOrDefault(m => m.Name == requestedName);
        string resolvedName = requestedName;
        if (method is null && !requestedName.StartsWith("set_", StringComparison.Ordinal))
        {
            string withSetPrefix = "set_" + requestedName;
            method = obj.Type.Methods.FirstOrDefault(m => m.Name == withSetPrefix);
            if (method is not null)
            {
                resolvedName = withSetPrefix;
            }
        }
        if (method is null)
        {
            throw new ClrSessionException(
                $"Methode CLR introuvable sur {obj.Type.Name} : {requestedName} (ni 'set_{requestedName}').");
        }

        bool isStatic = (method.Attributes & System.Reflection.MethodAttributes.Static) != 0;
        if (isStatic)
        {
            throw new ClrSessionException(
                $"Methode statique non supportee : {obj.Type.Name}.{resolvedName}. " +
                "Seuls les setters d'INSTANCE sont geres en v1 (this en RCX).");
        }

        (int parameterCount, string? parameterType) = ParseInstanceMethodParameters(method.Signature, resolvedName, obj.Type.Name ?? "");
        if (parameterCount > 1)
        {
            throw new ClrSessionException(
                $"Signature non supportee : {obj.Type.Name}.{resolvedName} attend {parameterCount} parametres. " +
                "Seuls les setters a 0 ou 1 parametre sont geres en v1.");
        }

        // Chantier "setters a parametre objet/string" : un parametre non
        // primitif (jamais en collision avec SupportedInstanceMethodParameterTypes,
        // voir le commentaire au-dessus de cette liste -- les primitifs sont
        // toujours rapportes en nom court par ClrMethod.Signature, les types
        // non primitifs toujours prefixes de leur namespace complet) doit
        // etre resolu en ClrType REEL pour determiner s'il s'agit d'un type
        // REFERENCE (classe/string/interface -- accepte, RDX porte l'adresse
        // brute) ou d'un type VALEUR (struct -- accepte depuis PHASE 76 dans
        // le cas taille 1/2/4/8 octets tous champs primitifs, RDX porte alors
        // les octets du struct empaquetes ; rejete explicitement au-dela,
        // voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md). Verifie par attache ClrMD reelle
        // avant d'ecrire ce code (pas devine) : ClrMethod n'expose QUE
        // Signature (string) pour un parametre, aucune API de resolution de
        // type de parametre -- mais heap.GetTypeByName(nomComplet) resout
        // avec succes un ClrType reel a partir du nom fourni par la
        // signature (confirme pour "KillEngine.ClrTestTarget.Item",
        // "KillEngine.ClrTestTarget.Coordinates" -- struct -- et
        // "System.String"), avec IsValueType fiable dans les deux cas.
        bool parameterIsReferenceType = false;
        // PHASE 76 : un parametre struct est desormais accepte dans UN cas
        // precis -- taille totale exactement 1/2/4/8 octets ET tous ses
        // champs d'instance primitifs -- car la convention d'appel x64
        // Windows passe alors le struct PAR VALEUR dans un unique registre
        // (RDX), exactement le meme mecanisme deja cable pour un parametre
        // primitif (buildCallInstanceMethodShellcode cote natif n'a besoin
        // d'aucun changement : seul l'immediate RDX differe). Un struct plus
        // grand ou contenant un champ non primitif (nested struct/reference)
        // passerait par un pointeur cache vers une copie -- ce second cas
        // reste hors scope, garde-fou explicite plus bas.
        List<object>? parameterStructFields = null;
        int parameterStructSize = 0;
        if (parameterType is not null && !SupportedInstanceMethodParameterTypes.Contains(parameterType))
        {
            ClrType? resolvedParameterType = runtime.Heap.GetTypeByName(parameterType);
            if (resolvedParameterType is null)
            {
                throw new ClrSessionException(
                    $"Type de parametre non resolu : {obj.Type.Name}.{resolvedName}({parameterType}). " +
                    "Ni primitif reconnu ni type reference resolvable via le tas CLR -- generique/non charge ? " +
                    "Non supporte.");
            }
            if (resolvedParameterType.IsValueType)
            {
                List<ClrInstanceField> fields = resolvedParameterType.Fields.ToList();
                List<string> nonPrimitive = fields.Where(f => !IsWritablePrimitive(f.ElementType)).Select(f => f.Name!).ToList();
                if (nonPrimitive.Count > 0)
                {
                    throw new ClrSessionException(
                        $"Type de parametre struct non supporte : {obj.Type.Name}.{resolvedName}({parameterType}) -- " +
                        $"champ(s) non primitif(s) {string.Join(", ", nonPrimitive)} (struct-dans-struct ou reference). " +
                        "Seul un struct dont TOUS les champs sont primitifs est supporte.");
                }
                int totalSize = fields.Count == 0 ? 0 : fields.Max(f => f.Offset + f.Size);
                if (totalSize is not (1 or 2 or 4 or 8))
                {
                    throw new ClrSessionException(
                        $"Type de parametre struct non supporte : {obj.Type.Name}.{resolvedName}({parameterType}) fait " +
                        $"{totalSize} octet(s) -- seule une taille de 1/2/4/8 octets (passage PAR REGISTRE selon la " +
                        "convention x64 Windows) est geree ; un struct plus grand passe par un pointeur cache vers " +
                        "une copie, hors scope de ce chantier.");
                }
                parameterStructSize = totalSize;
                parameterStructFields = fields
                    .Select(f => (object)new { name = f.Name, elementType = f.ElementType.ToString(), offset = f.Offset, size = f.Size })
                    .ToList();
            }
            else
            {
                parameterIsReferenceType = true;
            }
        }

        // ClrMD reel rapporte ulong.MaxValue (pas 0) pour une methode jamais
        // JITtee -- verifie par attache reelle sur ce process de test avant
        // d'ecrire cette condition (pas devine). On tolere les deux valeurs
        // au cas ou une version future de ClrMD revienne a 0.
        if (method.NativeCode == 0 || method.NativeCode == ulong.MaxValue)
        {
            throw new ClrSessionException(
                $"Le setter {obj.Type.Name}.{resolvedName} n'a jamais ete appele par le jeu -- impossible de resoudre " +
                "son adresse native. Declenche-le au moins une fois en jeu avant de reessayer.");
        }

        return new
        {
            success = true,
            objectAddress = ToHex(obj.Address),
            typeName = obj.Type.Name,
            methodName = resolvedName,
            nativeCodeAddress = ToHex(method.NativeCode),
            parameterType,
            parameterIsReferenceType,
            parameterIsStruct = parameterStructFields is not null,
            parameterStructSize,
            parameterStructFields,
            isStatic,
        };
    }

    private static (int ParameterCount, string? ParameterType) ParseInstanceMethodParameters(string? signature, string methodName, string typeName)
    {
        if (string.IsNullOrEmpty(signature))
        {
            throw new ClrSessionException($"Signature CLR indisponible pour {typeName}.{methodName}.");
        }

        int parenStart = signature.IndexOf('(');
        int parenEnd = signature.LastIndexOf(')');
        if (parenStart < 0 || parenEnd <= parenStart)
        {
            throw new ClrSessionException($"Signature CLR illisible pour {typeName}.{methodName} : '{signature}'.");
        }

        string paramList = signature.Substring(parenStart + 1, parenEnd - parenStart - 1).Trim();
        if (paramList.Length == 0)
        {
            return (0, null);
        }

        // Split naif sur ", " -- suffisant pour distinguer 0/1/plus-de-1
        // parametre. Un type generique avec virgule interne (ex:
        // Dictionary<K,V>, hors scope de toute facon) serait compte comme
        // plusieurs parametres et rejete par le garde-fou "0 ou 1 parametre"
        // -- resultat final correct (rejet) meme si le compte rapporte
        // serait techniquement inexact dans ce cas marginal.
        string[] parts = paramList.Split(", ", StringSplitOptions.None);
        return parts.Length == 1 ? (1, parts[0].Trim()) : (parts.Length, null);
    }

    public object WritePrimitiveField(string objectAddressHex, string fieldName, string valueText)
    {
        var runtime = RequireRuntime();
        if (_attachedPid is null)
        {
            throw new ClrSessionException("Aucun PID attache pour l'ecriture de champ CLR.");
        }

        ulong objectAddress = ParseHexAddress(objectAddressHex);
        ClrObject obj = runtime.Heap.GetObject(objectAddress);
        if (obj.IsNull || !obj.IsValid || obj.Type is null)
        {
            throw new ClrSessionException($"Adresse {objectAddressHex} : objet invalide ou null avant ecriture.");
        }

        return WriteFieldOnObject(obj, fieldName, valueText, path: fieldName);
    }

    public object WritePrimitivePath(string objectAddressHex, string path, string valueText)
    {
        var runtime = RequireRuntime();
        if (_attachedPid is null)
        {
            throw new ClrSessionException("Aucun PID attache pour l'ecriture de chemin CLR.");
        }

        ulong objectAddress = ParseHexAddress(objectAddressHex);
        ClrObject rootObj = runtime.Heap.GetObject(objectAddress);
        if (rootObj.IsNull || !rootObj.IsValid || rootObj.Type is null)
        {
            throw new ClrSessionException($"Adresse {objectAddressHex} : objet racine invalide ou null avant ecriture.");
        }

        IReadOnlyList<PathSegment> segments = ParsePath(path);
        if (segments[^1].Key is not null)
        {
            PathNode dictionaryOwner = ResolvePathOwner(rootObj, segments);
            return WriteDictionaryPrimitiveValue(dictionaryOwner, segments[^1], valueText, path);
        }

        PathNode owner = ResolvePathOwner(rootObj, segments);
        PathSegment leaf = segments[^1];
        if (leaf.Index is not null)
        {
            // PHASE 59 : un chemin indexe peut cibler soit un tableau/List<T>
            // de REFERENCES (deja supporte, WriteIndexedReferenceValue) soit
            // desormais un tableau/List<T> de PRIMITIFS (ex: int[] Scores) --
            // dispatch selon le type d'element reel du champ collection avant
            // d'ecrire, pas de melange des deux chemins.
            if (IsIndexedFieldPrimitiveArray(owner, leaf))
            {
                return WriteIndexedPrimitiveValue(owner, leaf, valueText, path);
            }
            // PHASE 76 : un chemin indexe peut aussi cibler un tableau/List<T>
            // de STRUCTS (ex: Coordinates[] Waypoints) -- ecrire l'element
            // ENTIER par index, plutot que rejeter (WriteIndexedReferenceValue
            // suppose une reference d'objet, pas une valeur inline).
            if (IsIndexedFieldStructArray(owner, leaf))
            {
                return WriteIndexedStructValue(owner, leaf, valueText, path);
            }
            return WriteIndexedReferenceValue(owner, leaf, valueText, path);
        }

        return WriteFieldOnPathNode(owner, leaf.FieldName, valueText, path);
    }

    public object WritePrimitivePathBatch(string objectAddressHex, IReadOnlyList<PathWriteOperation> operations)
    {
        if (operations.Count == 0)
        {
            throw new ClrSessionException("Transaction CLR vide.");
        }
        if (operations.Count > 32)
        {
            throw new ClrSessionException("Transaction CLR trop grande (max 32 operations).");
        }

        var applied = new List<(string Path, string OldValue)>();
        var results = new List<object>();
        try
        {
            foreach (PathWriteOperation operation in operations)
            {
                string path = operation.Path.Trim();
                string value = operation.Value.Trim();
                if (path.Length == 0 || value.Length == 0)
                {
                    throw new ClrSessionException("Chaque operation CLR doit fournir path et value.");
                }

                string oldValue = ReadPathValueAsWriteText(objectAddressHex, path);
                object result = WritePrimitivePath(objectAddressHex, path, value);
                bool verified = Convert.ToBoolean(result.GetType().GetProperty("verified")?.GetValue(result), CultureInfo.InvariantCulture);
                if (!verified)
                {
                    throw new ClrSessionException($"Verification echouee apres ecriture CLR : {path}");
                }

                applied.Add((path, oldValue));
                results.Add(result);
            }

            return new { success = true, applied = results.Count, rolledBack = false, results };
        }
        catch (Exception ex) when (ex is ClrSessionException or FormatException or OverflowException)
        {
            var rollbackErrors = new List<string>();
            for (int i = applied.Count - 1; i >= 0; --i)
            {
                try
                {
                    WritePrimitivePath(objectAddressHex, applied[i].Path, applied[i].OldValue);
                }
                catch (Exception rollbackEx)
                {
                    rollbackErrors.Add($"{applied[i].Path}: {rollbackEx.Message}");
                }
            }

            return new
            {
                success = false,
                appliedBeforeFailure = applied.Count,
                rolledBack = rollbackErrors.Count == 0,
                error = ex.Message,
                rollbackErrors,
            };
        }
    }

    private string ReadPathValueAsWriteText(string objectAddressHex, string path)
    {
        var runtime = RequireRuntime();
        ulong objectAddress = ParseHexAddress(objectAddressHex);
        ClrObject rootObj = runtime.Heap.GetObject(objectAddress);
        if (rootObj.IsNull || !rootObj.IsValid || rootObj.Type is null)
        {
            throw new ClrSessionException($"Adresse {objectAddressHex} : objet racine invalide ou null avant lecture transactionnelle.");
        }

        IReadOnlyList<PathSegment> segments = ParsePath(path);
        PathSegment leaf = segments[^1];
        if (leaf.Key is not null)
        {
            PathNode dictionaryOwner = ResolvePathOwner(rootObj, segments);
            object value = ReadDictionaryPrimitiveValue(dictionaryOwner, leaf);
            return ValueToWriteText(value);
        }

        PathNode owner = ResolvePathOwner(rootObj, segments);
        if (leaf.Index is not null)
        {
            if (IsIndexedFieldPrimitiveArray(owner, leaf))
            {
                object value = ReadIndexedPrimitiveValue(owner, leaf);
                return ValueToWriteText(value);
            }
            if (IsIndexedFieldStructArray(owner, leaf))
            {
                return ReadIndexedStructValueAsWriteText(owner, leaf);
            }
            ClrObject readBack = ReadIndexedReferenceValue(owner, leaf);
            return readBack.IsNull ? "null" : ToHex(readBack.Address);
        }

        if (owner.Object is ClrObject obj)
        {
            ClrInstanceField? field = obj.Type is null ? null : FindField(obj.Type, leaf.FieldName);
            if (field is null)
            {
                throw new ClrSessionException($"Champ CLR introuvable sur {obj.Type?.Name}: {leaf.FieldName}");
            }
            if (field.IsObjectReference && field.ElementType != ClrElementType.String)
            {
                ClrObject refObj = obj.ReadObjectField(field.Name!);
                return refObj.IsNull ? "null" : ToHex(refObj.Address);
            }
            return ValueToWriteText(ReadFieldValue(obj, field));
        }

        if (owner.ValueType is ClrValueType valueType)
        {
            ClrInstanceField? field = valueType.Type is null ? null : FindField(valueType.Type, leaf.FieldName);
            if (field is null)
            {
                throw new ClrSessionException($"Champ CLR introuvable sur {valueType.Type?.Name}: {leaf.FieldName}");
            }
            if (!IsWritablePrimitive(field.ElementType))
            {
                throw new ClrSessionException($"Champ struct non restaurable : {leaf.FieldName} ({field.ElementType}).");
            }
            return ValueToWriteText(ReadValueTypePrimitive(valueType, field));
        }

        throw new ClrSessionException($"Chemin CLR non restaurable : {path}");
    }

    private static string ValueToWriteText(object? value)
    {
        return value switch
        {
            null => "null",
            bool b => b ? "true" : "false",
            IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
            _ => Convert.ToString(value, CultureInfo.InvariantCulture) ?? "",
        };
    }

    private object WriteFieldOnPathNode(PathNode owner, string fieldName, string valueText, string path)
    {
        if (owner.Object is ClrObject obj)
        {
            return WriteFieldOnObject(obj, fieldName, valueText, path);
        }
        if (owner.ValueType is ClrValueType value)
        {
            return WriteFieldOnValueType(value, fieldName, valueText, path);
        }
        throw new ClrSessionException($"Chemin CLR invalide avant ecriture du champ {fieldName}.");
    }

    private object WriteFieldOnObject(ClrObject obj, string fieldName, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture de champ CLR.");
        if (obj.IsNull || !obj.IsValid || obj.Type is null)
        {
            throw new ClrSessionException($"Objet CLR invalide avant ecriture du champ {fieldName}.");
        }

        ClrInstanceField? field = FindField(obj.Type, fieldName);
        if (field is null)
        {
            throw new ClrSessionException($"Champ CLR introuvable sur {obj.Type.Name} : {fieldName}");
        }

        ulong fieldAddress = field.GetAddress(obj.Address);
        if (fieldAddress == 0)
        {
            throw new ClrSessionException($"Adresse effective du champ {field.Name} introuvable.");
        }

        if (IsWritablePrimitive(field.ElementType))
        {
            return WritePrimitiveAtAddress(attachedPid, obj, field, fieldAddress, valueText, path);
        }

        if (field.ElementType == ClrElementType.String)
        {
            ClrObject stringObject = obj.ReadObjectField(field.Name!);
            return WriteStringObjectInPlace(attachedPid, stringObject, valueText, path, field.Name!, ToHex(obj.Address), obj.Type.Name ?? "");
        }

        if (field.IsObjectReference)
        {
            ulong reference = ParseReferenceValue(valueText);
            WriteRawBytes(attachedPid, fieldAddress, EncodePointer(reference), $"reference CLR {field.Name}");
            ClrObject readBack = obj.ReadObjectField(field.Name!);
            return new
            {
                objectAddress = ToHex(obj.Address),
                typeName = obj.Type.Name,
                fieldName = field.Name,
                path,
                fieldAddress = ToHex(fieldAddress),
                elementType = field.ElementType.ToString(),
                bytesWritten = IntPtr.Size,
                value = readBack.IsNull ? null : DescribeObjectReference(readBack, depth: 0),
                verified = reference == 0 ? readBack.IsNull : readBack.Address == reference,
            };
        }

        throw new ClrSessionException($"Champ {field.Name} non supporte en ecriture : {field.ElementType}.");
    }

    private object WriteFieldOnValueType(ClrValueType valueType, string fieldName, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture de champ CLR.");
        if (!valueType.IsValid || valueType.Type is null)
        {
            throw new ClrSessionException($"Value-type CLR invalide avant ecriture du champ {fieldName}.");
        }

        ClrInstanceField? field = FindField(valueType.Type, fieldName);
        if (field is null)
        {
            throw new ClrSessionException($"Champ CLR introuvable sur {valueType.Type.Name} : {fieldName}");
        }
        if (!IsWritablePrimitive(field.ElementType))
        {
            throw new ClrSessionException($"Champ struct {field.Name} non supporte en ecriture : {field.ElementType}.");
        }

        ulong fieldAddress = field.GetAddress(valueType.Address, interior: true);
        if (fieldAddress == 0)
        {
            throw new ClrSessionException($"Adresse effective du champ struct {field.Name} introuvable.");
        }

        byte[] bytes = EncodePrimitive(field.ElementType, valueText);
        WriteRawBytes(attachedPid, fieldAddress, bytes, $"champ struct CLR {field.Name}");
        object readBack = ReadValueTypePrimitive(valueType, field);
        return new
        {
            objectAddress = ToHex(valueType.Address),
            typeName = valueType.Type.Name,
            fieldName = field.Name,
            path,
            fieldAddress = ToHex(fieldAddress),
            elementType = field.ElementType.ToString(),
            bytesWritten = bytes.Length,
            value = readBack,
            verified = PrimitiveMatches(field.ElementType, valueText, readBack),
        };
    }

    private object WritePrimitiveAtAddress(int attachedPid, ClrObject obj, ClrInstanceField field, ulong fieldAddress, string valueText, string path)
    {
        byte[] bytes = EncodePrimitive(field.ElementType, valueText);
        WriteRawBytes(attachedPid, fieldAddress, bytes, $"champ CLR {field.Name}");
        object? readBack = ReadFieldValue(obj, field);
        return new
        {
            objectAddress = ToHex(obj.Address),
            typeName = obj.Type?.Name ?? "",
            fieldName = field.Name,
            path,
            fieldAddress = ToHex(fieldAddress),
            elementType = field.ElementType.ToString(),
            bytesWritten = bytes.Length,
            value = readBack,
            verified = PrimitiveMatches(field.ElementType, valueText, readBack),
        };
    }

    private object WriteDictionaryPrimitiveValue(PathNode owner, PathSegment segment, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture de dictionnaire CLR.");
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName} doit appartenir a un objet.");
        }
        if (segment.Key is null)
        {
            throw new ClrSessionException($"Cle dictionnaire manquante pour {segment.FieldName}.");
        }

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            throw new ClrSessionException($"Champ dictionnaire CLR introuvable ou non reference : {segment.FieldName}");
        }

        ClrObject dictionary = obj.ReadObjectField(field.Name!);
        if (dictionary.IsNull || dictionary.Type is null)
        {
            throw new ClrSessionException($"Dictionnaire CLR null : {segment.FieldName}");
        }
        if (!(dictionary.Type.Name ?? "").StartsWith("System.Collections.Generic.Dictionary<", StringComparison.Ordinal))
        {
            throw new ClrSessionException($"{segment.FieldName} n'est pas un Dictionary<K,V> supporte.");
        }

        ClrValueType entry = FindDictionaryEntry(dictionary, segment.Key);
        ClrInstanceField? valueField = entry.Type?.GetFieldByName("value");
        if (valueField is null || !IsWritablePrimitive(valueField.ElementType))
        {
            throw new ClrSessionException($"Valeur du dictionnaire {segment.FieldName}[{segment.Key}] non primitive ou introuvable.");
        }

        ulong valueAddress = valueField.GetAddress(entry.Address, interior: true);
        if (valueAddress == 0)
        {
            throw new ClrSessionException($"Adresse de valeur dictionnaire introuvable pour {segment.FieldName}[{segment.Key}].");
        }

        byte[] bytes = EncodePrimitive(valueField.ElementType, valueText);
        WriteRawBytes(attachedPid, valueAddress, bytes, $"valeur dictionnaire CLR {segment.FieldName}[{segment.Key}]");
        object readBack = ReadValueTypePrimitive(entry, valueField);
        return new
        {
            objectAddress = ToHex(dictionary.Address),
            typeName = dictionary.Type.Name,
            fieldName = "value",
            path,
            fieldAddress = ToHex(valueAddress),
            elementType = valueField.ElementType.ToString(),
            bytesWritten = bytes.Length,
            value = readBack,
            verified = PrimitiveMatches(valueField.ElementType, valueText, readBack),
        };
    }

    private static object ReadDictionaryPrimitiveValue(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Key}] doit appartenir a un objet.");
        }
        if (segment.Key is null)
        {
            throw new ClrSessionException($"Cle dictionnaire manquante pour {segment.FieldName}.");
        }

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            throw new ClrSessionException($"Champ dictionnaire CLR introuvable ou non reference : {segment.FieldName}");
        }

        ClrObject dictionary = obj.ReadObjectField(field.Name!);
        if (dictionary.IsNull || dictionary.Type is null)
        {
            throw new ClrSessionException($"Dictionnaire CLR null : {segment.FieldName}");
        }

        ClrValueType entry = FindDictionaryEntry(dictionary, segment.Key);
        ClrInstanceField? valueField = entry.Type?.GetFieldByName("value");
        if (valueField is null || !IsWritablePrimitive(valueField.ElementType))
        {
            throw new ClrSessionException($"Valeur du dictionnaire {segment.FieldName}[{segment.Key}] non primitive ou introuvable.");
        }
        return ReadValueTypePrimitive(entry, valueField);
    }

    // PHASE 59 : reconnait un chemin indexe qui cible un tableau/List<T> dont
    // l'ELEMENT est un primitif (bool/int8..64/uint8..64/float/double), pas
    // une reference d'objet ni un struct. Utilise seulement pour le DERNIER
    // segment du chemin (l'index est la feuille, ex: Scores[2]) -- quand
    // l'index n'est PAS la feuille (ex: Waypoints[1].X), c'est
    // ResolveIndexedReference/ResolveArrayElementNode qui gerent la
    // composition tableau-de-structs, chantier "ecriture indexee dans des
    // tableaux de STRUCTS" (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) -- un
    // champ primitif a l'interieur d'un element struct de tableau
    // (Champ[i].SousChamp) est supporte, un seul niveau de struct verifie
    // par un test dedie (niveaux plus profonds, ex: struct-dans-struct-
    // dans-tableau, supportes par la meme composition generique de
    // PathNode/ResolvePathSegment mais pas explicitement testes pour ce cas
    // precis dans ce lot). Ecrire l'element de tableau de structs ENTIER
    // par index (Waypoints[1] seul, sans champ suivant) est desormais
    // supporte aussi, voir IsIndexedFieldStructArray/WriteIndexedStructValue
    // plus bas -- limite a un struct dont TOUS les champs sont primitifs
    // (struct-dans-struct-dans-tableau reste hors scope pour cette ecriture
    // "element entier" specifiquement).
    private static bool IsIndexedFieldPrimitiveArray(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            return false;
        }

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            return false;
        }

        ClrObject collection = obj.ReadObjectField(field.Name!);
        if (collection.IsNull || collection.Type is null)
        {
            return false;
        }

        ClrType? componentType = null;
        if (collection.Type.IsArray)
        {
            componentType = collection.Type.ComponentType;
        }
        else if ((collection.Type.Name ?? "").StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            ClrObject items = collection.ReadObjectField("_items");
            if (!items.IsNull && items.Type is not null && items.Type.IsArray)
            {
                componentType = items.Type.ComponentType;
            }
        }

        return componentType is not null
            && IsPrimitiveElement(componentType.ElementType)
            && componentType.ElementType != ClrElementType.Char;
    }

    // Chantier "ecriture d'un element de tableau/List<T> de STRUCTS ENTIER
    // par index" (docs/POWER_UP_ROADMAP.md candidat #8, extension listee
    // "non couverte a ce jour") -- meme forme que IsIndexedFieldPrimitiveArray
    // ci-dessus mais reconnait l'inverse : l'element est un VALUE TYPE non
    // primitif (struct), pas un tableau de primitifs ni de references.
    private static bool IsIndexedFieldStructArray(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            return false;
        }

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            return false;
        }

        ClrObject collection = obj.ReadObjectField(field.Name!);
        if (collection.IsNull || collection.Type is null)
        {
            return false;
        }

        ClrType? componentType = null;
        if (collection.Type.IsArray)
        {
            componentType = collection.Type.ComponentType;
        }
        else if ((collection.Type.Name ?? "").StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            ClrObject items = collection.ReadObjectField("_items");
            if (!items.IsNull && items.Type is not null && items.Type.IsArray)
            {
                componentType = items.Type.ComponentType;
            }
        }

        return componentType is not null && componentType.IsValueType && !IsPrimitiveElement(componentType.ElementType);
    }

    /// <summary>
    /// Ecriture directe par index dans un tableau/List<T> de PRIMITIFS (ex:
    /// int[] Scores, Scores[2] = 42) -- extension PHASE 59 de
    /// writePrimitivePath. Resout l'adresse de l'element via
    /// ClrType.GetArrayElementAddress, la meme primitive deja utilisee cote
    /// LECTURE (DescribeArray/ReadArrayElement) et cote ecriture de reference
    /// (WriteIndexedReferenceValue), ecrit via WriteProcessMemory comme le
    /// reste du module, puis relit pour verifier.
    /// </summary>
    private object WriteIndexedPrimitiveValue(PathNode owner, PathSegment segment, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture d'element de tableau CLR.");
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de tableau CLR manquant.");

        (ClrObject arrayObject, int logicalLength) = ResolveIndexedPrimitiveCollection(obj, segment.FieldName);
        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }

        ClrType? componentType = arrayObject.Type?.ComponentType;
        if (componentType is null || !IsPrimitiveElement(componentType.ElementType) || componentType.ElementType == ClrElementType.Char)
        {
            throw new ClrSessionException(
                $"Element {segment.FieldName}[{index}] non primitif ecrivable directement en tant que FEUILLE du chemin -- " +
                "un tableau de STRUCTS (element entier ou champ interieur) et un tableau de REFERENCES passent par un " +
                $"autre chemin d'ecriture ; verifie {segment.FieldName}[{index}] (element entier) ou " +
                $"{segment.FieldName}[{index}].NomDuChamp (champ interieur) (voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md).");
        }

        ulong elementAddress = arrayObject.Type!.GetArrayElementAddress(arrayObject.Address, index);
        if (elementAddress == 0)
        {
            throw new ClrSessionException($"Adresse de l'element {segment.FieldName}[{index}] introuvable.");
        }

        byte[] bytes = EncodePrimitive(componentType.ElementType, valueText);
        WriteRawBytes(attachedPid, elementAddress, bytes, $"element primitif CLR {segment.FieldName}[{index}]");
        object readBack = ReadArrayPrimitive(arrayObject.AsArray(), componentType.ElementType, index);
        return new
        {
            objectAddress = ToHex(arrayObject.Address),
            typeName = arrayObject.Type!.Name,
            fieldName = segment.FieldName,
            path,
            fieldAddress = ToHex(elementAddress),
            elementType = componentType.ElementType.ToString(),
            bytesWritten = bytes.Length,
            value = readBack,
            verified = PrimitiveMatches(componentType.ElementType, valueText, readBack),
        };
    }

    /// <summary>
    /// Ecriture ENTIERE d'un element de tableau/List&lt;T&gt; de STRUCTS par
    /// index (ex: Coordinates[] Waypoints, Waypoints[1] = nouvel element) --
    /// dernier point de l'extension "ecriture indexee dans des tableaux de
    /// STRUCTS" (PHASE 59/66) laisse ouvert faute de "valeur primitive
    /// unique a encoder" et de format de saisie defini. Format retenu :
    /// "Champ1=Valeur1,Champ2=Valeur2" -- TOUS les champs d'instance du
    /// struct doivent etre fournis (semantique de remplacement complet de
    /// l'element, pas une mise a jour partielle silencieuse). Struct-dans-
    /// struct-dans-tableau (un champ non primitif A L'INTERIEUR du struct
    /// element) reste explicitement hors scope de cette methode -- rejet
    /// propre plutot qu'une ecriture partielle/incorrecte.
    /// </summary>
    // Chantier "struct-dans-tableau-de-structs en ecriture ENTIERE" (docs/
    // POWER_UP_ROADMAP.md, "Extensions futures non bloquantes") : au depart
    // WriteIndexedStructValue exigeait que TOUS les champs du struct soient
    // primitifs (un seul niveau) -- un struct contenant lui-meme un struct
    // imbrique (ex: Zone { Coordinates Origin; int Radius; }) etait rejete.
    // Etendu pour deballer recursivement les champs struct imbriques jusqu'a
    // MaxValueTypeDepth (meme borne que le deballage en LECTURE,
    // ReadValueTypeFieldValue) -- une reference (objet/string) a l'interieur
    // reste explicitement hors scope (pas de chemin d'ecriture symbolique
    // pour un struct entier contenant une reference, contrairement au champ
    // isole via writePrimitivePath). Format de saisie retenu : cles A PLAT
    // avec chemin en points ("Origin.X=100,Origin.Y=200,Radius=5"), coherent
    // avec la syntaxe deja utilisee pour les chemins de champ (Stats.HomeZone.Origin.X).
    private readonly record struct StructLeaf(string DottedName, IReadOnlyList<ClrInstanceField> FieldChain, ulong Address);

    private static void CollectWritableStructLeaves(
        ClrType structType,
        ulong structAddress,
        string prefix,
        IReadOnlyList<ClrInstanceField> chain,
        int depth,
        List<StructLeaf> leaves)
    {
        foreach (ClrInstanceField field in structType.Fields)
        {
            string name = prefix.Length == 0 ? field.Name! : $"{prefix}.{field.Name}";
            var nextChain = new List<ClrInstanceField>(chain) { field };
            ulong fieldAddress = field.GetAddress(structAddress, interior: true);

            if (IsWritablePrimitive(field.ElementType))
            {
                leaves.Add(new StructLeaf(name, nextChain, fieldAddress));
            }
            else if (field.Type?.IsValueType == true)
            {
                if (depth >= MaxValueTypeDepth)
                {
                    throw new ClrSessionException(
                        $"Ecriture d'element struct ENTIER : profondeur maximale de struct imbriquee ({MaxValueTypeDepth}) atteinte pour {name}.");
                }
                CollectWritableStructLeaves(field.Type, fieldAddress, name, nextChain, depth + 1, leaves);
            }
            else
            {
                throw new ClrSessionException(
                    $"Ecriture d'element struct ENTIER : champ {name} n'est ni primitif ni struct imbrique (reference/collection) -- " +
                    "non supporte par cette ecriture (extension separee, hors scope).");
            }
        }
    }

    private static object ReadStructLeafValue(ClrValueType root, IReadOnlyList<ClrInstanceField> chain)
    {
        ClrValueType current = root;
        for (int i = 0; i < chain.Count - 1; i++)
        {
            current = current.ReadValueTypeField(chain[i]);
        }
        return ReadValueTypePrimitive(current, chain[^1]);
    }

    private object WriteIndexedStructValue(PathNode owner, PathSegment segment, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture d'element de tableau CLR.");
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de tableau CLR manquant.");

        (ClrObject arrayObject, int logicalLength) = ResolveIndexedPrimitiveCollection(obj, segment.FieldName);
        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }

        ClrType? componentType = arrayObject.Type?.ComponentType;
        if (componentType is null || !componentType.IsValueType || IsPrimitiveElement(componentType.ElementType))
        {
            throw new ClrSessionException($"Element {segment.FieldName}[{index}] n'est pas un struct.");
        }

        ulong elementAddress = arrayObject.Type!.GetArrayElementAddress(arrayObject.Address, index);
        if (elementAddress == 0)
        {
            throw new ClrSessionException($"Adresse de l'element {segment.FieldName}[{index}] introuvable.");
        }

        var leaves = new List<StructLeaf>();
        CollectWritableStructLeaves(componentType, elementAddress, string.Empty, Array.Empty<ClrInstanceField>(), 0, leaves);

        Dictionary<string, string> assignments = ParseStructFieldAssignments(valueText);

        List<string> missing = leaves.Select(l => l.DottedName).Where(name => !assignments.ContainsKey(name)).ToList();
        if (missing.Count > 0)
        {
            throw new ClrSessionException(
                $"Ecriture de {segment.FieldName}[{index}] : champ(s) manquant(s) {string.Join(", ", missing)} -- " +
                "l'ecriture d'un element struct ENTIER exige tous ses champs feuilles (format \"Champ1=Valeur1,Sous.Champ2=Valeur2\").");
        }
        List<string> unknown = assignments.Keys.Where(name => leaves.All(l => l.DottedName != name)).ToList();
        if (unknown.Count > 0)
        {
            throw new ClrSessionException($"Ecriture de {segment.FieldName}[{index}] : champ(s) inconnu(s) {string.Join(", ", unknown)}.");
        }

        var writtenFields = new List<object>();
        foreach (StructLeaf leaf in leaves)
        {
            if (leaf.Address == 0)
            {
                throw new ClrSessionException($"Adresse effective du champ {leaf.DottedName} introuvable dans {segment.FieldName}[{index}].");
            }
            ClrElementType elementType = leaf.FieldChain[^1].ElementType;
            byte[] bytes = EncodePrimitive(elementType, assignments[leaf.DottedName]);
            WriteRawBytes(attachedPid, leaf.Address, bytes, $"champ struct CLR {segment.FieldName}[{index}].{leaf.DottedName}");
            writtenFields.Add(new { name = leaf.DottedName, address = ToHex(leaf.Address), bytesWritten = bytes.Length });
        }

        ClrValueType readBack = arrayObject.AsArray().GetStructValue(index);
        var values = new Dictionary<string, object?>();
        bool verified = true;
        foreach (StructLeaf leaf in leaves)
        {
            object value = ReadStructLeafValue(readBack, leaf.FieldChain);
            values[leaf.DottedName] = value;
            if (!PrimitiveMatches(leaf.FieldChain[^1].ElementType, assignments[leaf.DottedName], value))
            {
                verified = false;
            }
        }

        return new
        {
            objectAddress = ToHex(arrayObject.Address),
            typeName = componentType.Name,
            fieldName = segment.FieldName,
            path,
            elementAddress = ToHex(elementAddress),
            fields = writtenFields,
            value = values,
            verified,
        };
    }

    private static Dictionary<string, string> ParseStructFieldAssignments(string valueText)
    {
        var result = new Dictionary<string, string>();
        foreach (string part in valueText.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            int separator = part.IndexOf('=');
            if (separator <= 0)
            {
                throw new ClrSessionException($"Format d'ecriture struct invalide (attendu \"Champ=Valeur\") : \"{part.Trim()}\".");
            }
            string name = part[..separator].Trim();
            string value = part[(separator + 1)..].Trim();
            if (name.Length == 0 || value.Length == 0)
            {
                throw new ClrSessionException($"Format d'ecriture struct invalide (attendu \"Champ=Valeur\") : \"{part.Trim()}\".");
            }
            result[name] = value;
        }
        if (result.Count == 0)
        {
            throw new ClrSessionException("Ecriture d'element struct : aucune assignation fournie (format attendu \"Champ1=Valeur1,Champ2=Valeur2\").");
        }
        return result;
    }

    // Symetrique de WriteIndexedStructValue, meme format texte
    // "Champ1=Valeur1,Champ2=Valeur2" -- utilise par WritePrimitivePathBatch
    // pour capturer la valeur AVANT ecriture et pouvoir la restaurer telle
    // quelle en cas de rollback transactionnel.
    private static string ReadIndexedStructValueAsWriteText(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de tableau CLR manquant.");

        (ClrObject arrayObject, int logicalLength) = ResolveIndexedPrimitiveCollection(obj, segment.FieldName);
        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }
        ClrType? componentType = arrayObject.Type?.ComponentType;
        if (componentType is null || !componentType.IsValueType || IsPrimitiveElement(componentType.ElementType))
        {
            throw new ClrSessionException($"Element {segment.FieldName}[{index}] n'est pas un struct restaurable.");
        }

        ClrValueType structValue = arrayObject.AsArray().GetStructValue(index);
        var parts = new List<string>();
        foreach (ClrInstanceField field in componentType.Fields)
        {
            if (!IsWritablePrimitive(field.ElementType))
            {
                throw new ClrSessionException($"Element {segment.FieldName}[{index}] : champ non primitif {field.Name}, non restaurable par ce chemin.");
            }
            parts.Add($"{field.Name}={ValueToWriteText(ReadValueTypePrimitive(structValue, field))}");
        }
        return string.Join(",", parts);
    }

    private static object ReadIndexedPrimitiveValue(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de tableau CLR manquant.");

        (ClrObject arrayObject, int logicalLength) = ResolveIndexedPrimitiveCollection(obj, segment.FieldName);
        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }

        ClrType? componentType = arrayObject.Type?.ComponentType;
        if (componentType is null || !IsPrimitiveElement(componentType.ElementType) || componentType.ElementType == ClrElementType.Char)
        {
            throw new ClrSessionException($"Element {segment.FieldName}[{index}] non primitif restaurable.");
        }

        return ReadArrayPrimitive(arrayObject.AsArray(), componentType.ElementType, index);
    }

    private static (ClrObject ArrayObject, int LogicalLength) ResolveIndexedPrimitiveCollection(ClrObject obj, string fieldName)
    {
        ClrInstanceField? field = FindField(obj.Type!, fieldName);
        if (field is null || !field.IsObjectReference)
        {
            throw new ClrSessionException($"Champ tableau CLR introuvable ou non reference : {fieldName}");
        }

        ClrObject collection = obj.ReadObjectField(field.Name!);
        if (collection.IsNull || collection.Type is null)
        {
            throw new ClrSessionException($"Tableau CLR null : {fieldName}");
        }

        if (collection.Type.IsArray)
        {
            return (collection, collection.AsArray().GetLength(0));
        }

        if ((collection.Type.Name ?? "").StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            int logicalLength = SafeReadIntField(collection, "_size");
            ClrObject items = collection.ReadObjectField("_items");
            if (items.IsNull || items.Type is null || !items.Type.IsArray)
            {
                throw new ClrSessionException($"Stockage interne de la liste {fieldName} introuvable.");
            }
            return (items, logicalLength);
        }

        throw new ClrSessionException($"{fieldName} n'est pas un tableau ou List<T> primitif supporte.");
    }

    private object WriteIndexedReferenceValue(PathNode owner, PathSegment segment, string valueText, string path)
    {
        int attachedPid = _attachedPid ?? throw new ClrSessionException("Aucun PID attache pour l'ecriture de reference CLR.");
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de reference CLR manquant.");

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            throw new ClrSessionException($"Champ collection CLR introuvable ou non reference : {segment.FieldName}");
        }

        ClrObject collection = obj.ReadObjectField(field.Name!);
        if (collection.IsNull || collection.Type is null)
        {
            throw new ClrSessionException($"Collection CLR null : {segment.FieldName}");
        }

        ClrObject arrayObject = collection;
        int logicalLength;
        if (collection.Type.IsArray)
        {
            logicalLength = collection.AsArray().GetLength(0);
        }
        else if ((collection.Type.Name ?? "").StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            logicalLength = SafeReadIntField(collection, "_size");
            arrayObject = collection.ReadObjectField("_items");
            if (arrayObject.IsNull || arrayObject.Type is null || !arrayObject.Type.IsArray)
            {
                throw new ClrSessionException($"Stockage interne de la liste {segment.FieldName} introuvable.");
            }
        }
        else
        {
            throw new ClrSessionException($"{segment.FieldName} n'est pas un tableau ou List<T> supporte.");
        }

        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }
        if (arrayObject.Type?.ComponentType is null || arrayObject.Type.ComponentType.ElementType == ClrElementType.String)
        {
            throw new ClrSessionException($"Element {segment.FieldName}[{index}] non supporte : seules les references objet sont modifiables ici.");
        }
        if (arrayObject.Type.ComponentType.IsValueType)
        {
            // Ne devrait normalement pas etre atteint : WritePrimitivePath
            // route deja un element de tableau de structs vers
            // WriteIndexedStructValue via IsIndexedFieldStructArray avant
            // d'appeler cette methode. Garde-fou defensif conserve au cas ou
            // un appelant futur invoquerait WriteIndexedReferenceValue
            // directement sans repasser par ce dispatch.
            throw new ClrSessionException(
                $"Element {segment.FieldName}[{index}] est un struct -- utilise l'ecriture d'element struct " +
                $"({segment.FieldName}[{index}] seul) ou un champ interieur ({segment.FieldName}[{index}].NomDuChamp), " +
                "pas cette methode (voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md).");
        }

        ulong reference = ParseReferenceValue(valueText);
        ulong elementAddress = arrayObject.Type.GetArrayElementAddress(arrayObject.Address, index);
        if (elementAddress == 0)
        {
            throw new ClrSessionException($"Adresse de l'element {segment.FieldName}[{index}] introuvable.");
        }

        WriteRawBytes(attachedPid, elementAddress, EncodePointer(reference), $"element reference CLR {segment.FieldName}[{index}]");
        ClrObject readBack = arrayObject.AsArray().GetObjectValue(index);
        return new
        {
            objectAddress = ToHex(arrayObject.Address),
            typeName = arrayObject.Type.Name,
            fieldName = segment.FieldName,
            path,
            fieldAddress = ToHex(elementAddress),
            elementType = arrayObject.Type.ComponentType.ElementType.ToString(),
            bytesWritten = IntPtr.Size,
            value = readBack.IsNull ? null : DescribeObjectReference(readBack, depth: 0),
            verified = reference == 0 ? readBack.IsNull : readBack.Address == reference,
        };
    }

    private static ClrObject ReadIndexedReferenceValue(PathNode owner, PathSegment segment)
    {
        if (owner.Object is not ClrObject obj || obj.IsNull || obj.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName}[{segment.Index}] doit appartenir a un objet.");
        }
        int index = segment.Index ?? throw new ClrSessionException("Index de reference CLR manquant.");

        ClrInstanceField? field = FindField(obj.Type, segment.FieldName);
        if (field is null || !field.IsObjectReference)
        {
            throw new ClrSessionException($"Champ collection CLR introuvable ou non reference : {segment.FieldName}");
        }

        ClrObject collection = obj.ReadObjectField(field.Name!);
        if (collection.IsNull || collection.Type is null)
        {
            throw new ClrSessionException($"Collection CLR null : {segment.FieldName}");
        }

        ClrObject arrayObject = collection;
        int logicalLength;
        if (collection.Type.IsArray)
        {
            logicalLength = collection.AsArray().GetLength(0);
        }
        else if ((collection.Type.Name ?? "").StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            logicalLength = SafeReadIntField(collection, "_size");
            arrayObject = collection.ReadObjectField("_items");
            if (arrayObject.IsNull || arrayObject.Type is null || !arrayObject.Type.IsArray)
            {
                throw new ClrSessionException($"Stockage interne de la liste {segment.FieldName} introuvable.");
            }
        }
        else
        {
            throw new ClrSessionException($"{segment.FieldName} n'est pas un tableau ou List<T> supporte.");
        }

        if (index < 0 || index >= logicalLength)
        {
            throw new ClrSessionException($"Index hors limites pour {segment.FieldName}[{index}] (taille {logicalLength}).");
        }
        return arrayObject.AsArray().GetObjectValue(index);
    }

    private static ClrValueType FindDictionaryEntry(ClrObject dictionaryObject, string key)
    {
        ClrObject entriesObject = dictionaryObject.ReadObjectField("_entries");
        if (entriesObject.IsNull || entriesObject.Type is null || !entriesObject.Type.IsArray)
        {
            throw new ClrSessionException("Table interne _entries du dictionnaire introuvable.");
        }

        ClrArray entriesArray = entriesObject.AsArray();
        int physicalLength = entriesArray.GetLength(0);
        for (int i = 0; i < physicalLength; ++i)
        {
            ClrValueType entry = entriesArray.GetStructValue(i);
            int hashCode = TryReadValueTypeIntField(entry, "hashCode", out int hc) ? hc : 0;
            if (hashCode < 0)
            {
                continue;
            }

            object? currentKey = ReadValueTypeFieldByName(entry, "key", depth: 0);
            if (string.Equals(Convert.ToString(currentKey, CultureInfo.InvariantCulture), key, StringComparison.Ordinal))
            {
                return entry;
            }
        }

        throw new ClrSessionException($"Cle dictionnaire introuvable : {key}");
    }

    private object WriteStringObjectInPlace(int attachedPid, ClrObject stringObject, string valueText, string path, string fieldName, string ownerAddress, string ownerTypeName)
    {
        if (stringObject.IsNull || stringObject.Type?.IsString != true)
        {
            throw new ClrSessionException($"Champ string {fieldName} null ou invalide.");
        }

        string current = stringObject.AsString(4096) ?? "";
        if (valueText.Length != current.Length)
        {
            throw new ClrSessionException(
                $"Ecriture string in-place refusee : longueur actuelle {current.Length}, nouvelle longueur {valueText.Length}. " +
                "Cette version ne change pas l'allocation ni la longueur d'une string managée.");
        }

        byte[] bytes = System.Text.Encoding.Unicode.GetBytes(valueText);
        ulong charsAddress = stringObject.Address + (ulong)IntPtr.Size + sizeof(int);
        WriteRawBytes(attachedPid, charsAddress, bytes, $"contenu string CLR {fieldName}");
        string readBack = stringObject.AsString(4096) ?? "";
        return new
        {
            objectAddress = ownerAddress,
            typeName = ownerTypeName,
            fieldName,
            path,
            fieldAddress = ToHex(charsAddress),
            elementType = ClrElementType.String.ToString(),
            bytesWritten = bytes.Length,
            value = readBack,
            verified = string.Equals(valueText, readBack, StringComparison.Ordinal),
            mode = "string_in_place_same_length",
        };
    }

    private static void WriteRawBytes(int attachedPid, ulong address, byte[] bytes, string label)
    {
        using SafeProcessHandle process = OpenProcess(ProcessVmWrite | ProcessVmOperation, false, attachedPid);
        if (process.IsInvalid)
        {
            throw new ClrSessionException($"OpenProcess pour ecriture CLR echoue (pid={attachedPid}, error={Marshal.GetLastWin32Error()}).");
        }

        if (!WriteProcessMemory(process, new IntPtr(unchecked((long)address)), bytes, bytes.Length, out nint bytesWritten)
            || bytesWritten.ToInt64() != bytes.Length)
        {
            throw new ClrSessionException($"WriteProcessMemory {label} echoue (adresse={ToHex(address)}, error={Marshal.GetLastWin32Error()}).");
        }
    }

    private static IReadOnlyList<PathSegment> ParsePath(string path)
    {
        string trimmed = path.Trim();
        if (trimmed.Length == 0)
        {
            throw new ClrSessionException("Chemin CLR vide.");
        }

        string[] rawSegments = trimmed.Split('.', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (rawSegments.Length == 0)
        {
            throw new ClrSessionException("Chemin CLR vide.");
        }

        var segments = new List<PathSegment>(rawSegments.Length);
        foreach (string raw in rawSegments)
        {
            int bracketStart = raw.IndexOf('[');
            if (bracketStart < 0)
            {
                segments.Add(new PathSegment(raw, null, null));
                continue;
            }

            if (!raw.EndsWith(']') || raw.IndexOf('[', bracketStart + 1) >= 0)
            {
                throw new ClrSessionException($"Segment de chemin CLR invalide : {raw}");
            }

            string fieldName = raw[..bracketStart].Trim();
            string indexText = raw[(bracketStart + 1)..^1].Trim();
            if (fieldName.Length == 0)
            {
                throw new ClrSessionException($"Segment de chemin CLR invalide : {raw}");
            }
            if (int.TryParse(indexText, NumberStyles.None, CultureInfo.InvariantCulture, out int index) && index >= 0)
            {
                segments.Add(new PathSegment(fieldName, index, null));
                continue;
            }

            string key = UnquoteDictionaryKey(indexText);
            if (key.Length == 0)
            {
                throw new ClrSessionException($"Cle de dictionnaire CLR invalide : {raw}");
            }
            segments.Add(new PathSegment(fieldName, null, key));
        }

        return segments;
    }

    private static string UnquoteDictionaryKey(string indexText)
    {
        string text = indexText.Trim();
        if (text.Length >= 2 && ((text[0] == '"' && text[^1] == '"') || (text[0] == '\'' && text[^1] == '\'')))
        {
            return text[1..^1];
        }
        return text;
    }

    private static PathNode ResolvePathOwner(ClrObject rootObj, IReadOnlyList<PathSegment> segments)
    {
        if (segments.Count == 1)
        {
            return PathNode.FromObject(rootObj);
        }

        PathNode current = PathNode.FromObject(rootObj);
        for (int i = 0; i < segments.Count - 1; ++i)
        {
            current = ResolvePathSegment(current, segments[i]);
        }
        return current;
    }

    private static PathNode ResolvePathSegment(PathNode current, PathSegment segment)
    {
        if (segment.Key is not null)
        {
            throw new ClrSessionException($"Cle dictionnaire autorisee seulement sur le dernier segment : {segment.FieldName}[{segment.Key}].");
        }

        if (current.Object is ClrObject obj)
        {
            return ResolveObjectPathSegment(obj, segment);
        }
        if (current.ValueType is ClrValueType valueType)
        {
            return ResolveValueTypePathSegment(valueType, segment);
        }
        throw new ClrSessionException($"Chemin CLR impossible avant {segment.FieldName}.");
    }

    private static PathNode ResolveObjectPathSegment(ClrObject current, PathSegment segment)
    {
        if (current.IsNull || current.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : objet null avant {segment.FieldName}.");
        }

        ClrInstanceField? field = FindField(current.Type, segment.FieldName);
        if (field is null)
        {
            throw new ClrSessionException($"Champ CLR introuvable sur {current.Type.Name} : {segment.FieldName}");
        }
        if (field.IsObjectReference)
        {
            ClrObject next = current.ReadObjectField(field.Name!);
            if (next.IsNull || next.Type is null)
            {
                throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName} est null.");
            }

            return segment.Index is null
                ? PathNode.FromObject(next)
                : ResolveIndexedReference(next, segment.Index.Value, segment.FieldName);
        }

        if (field.Type?.IsValueType == true)
        {
            if (segment.Index is not null)
            {
                throw new ClrSessionException($"Index non supporte sur le champ struct {segment.FieldName}.");
            }
            return PathNode.FromValueType(current.ReadValueTypeField(field.Name!));
        }

        throw new ClrSessionException(
            $"Chemin CLR non supporte : {segment.FieldName} est {field.ElementType}, pas une reference ou une struct traversable.");
    }

    private static PathNode ResolveValueTypePathSegment(ClrValueType current, PathSegment segment)
    {
        if (!current.IsValid || current.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : struct invalide avant {segment.FieldName}.");
        }

        ClrInstanceField? field = FindField(current.Type, segment.FieldName);
        if (field is null)
        {
            throw new ClrSessionException($"Champ CLR introuvable sur {current.Type.Name} : {segment.FieldName}");
        }
        if (field.IsObjectReference)
        {
            ClrObject next = current.ReadObjectField(field);
            if (next.IsNull || next.Type is null)
            {
                throw new ClrSessionException($"Chemin CLR impossible : {segment.FieldName} est null.");
            }
            return segment.Index is null
                ? PathNode.FromObject(next)
                : ResolveIndexedReference(next, segment.Index.Value, segment.FieldName);
        }
        if (field.Type?.IsValueType == true)
        {
            if (segment.Index is not null)
            {
                throw new ClrSessionException($"Index non supporte sur le champ struct {segment.FieldName}.");
            }
            return PathNode.FromValueType(current.ReadValueTypeField(field));
        }

        throw new ClrSessionException(
            $"Chemin CLR non supporte : {segment.FieldName} est {field.ElementType}, pas une reference ou une struct traversable.");
    }

    /// <summary>
    /// Resout un segment de chemin indexe (<c>Champ[i]</c>) qui n'est PAS le
    /// dernier segment du chemin -- l'appelant continue forcement par un
    /// autre segment (ex: <c>Waypoints[1].X</c>). Retourne un
    /// <see cref="PathNode"/> generique (objet OU struct) plutot qu'un
    /// <see cref="ClrObject"/> fixe -- chantier "ecriture indexee dans des
    /// tableaux de STRUCTS" (docs/KILLENGINE_CLR_INSPECTOR_SPEC.md) :
    /// jusqu'ici un tableau/List&lt;T&gt; dont l'ELEMENT est un struct
    /// (value-type non primitif) etait rejete explicitement des cette etape,
    /// meme quand le chemin continuait par un champ primitif a l'interieur
    /// de l'element (ex: <c>Points[2].X</c>). La composition necessaire
    /// existait deja separement ailleurs dans ce fichier (adresse d'element
    /// de tableau via <see cref="ClrType.GetArrayElementAddress"/>, adresse
    /// de champ dans un struct deja localise via
    /// <c>ClrInstanceField.GetAddress(structAddress, interior: true)</c>,
    /// reutilisee telle quelle par <see cref="WriteFieldOnValueType"/>) --
    /// il manquait seulement de les enchainer ici. Une fois ce noeud struct
    /// retourne, <see cref="ResolvePathSegment"/>/<see cref="WriteFieldOnPathNode"/>
    /// le traitent comme n'importe quel autre noeud struct deja supporte
    /// (y compris plusieurs niveaux de struct-dans-struct si le type le
    /// permet, sans code supplementaire) -- seul un tableau d'elements
    /// REFERENCE (classe/string) ou PRIMITIF continue d'emprunter les
    /// chemins dedies existants (<see cref="ResolveArrayElementNode"/>
    /// pour les references/structs en noeud intermediaire, <see cref="WriteIndexedPrimitiveValue"/>
    /// pour un primitif en feuille).
    /// </summary>
    private static PathNode ResolveIndexedReference(ClrObject collection, int index, string segmentName)
    {
        ClrType? type = collection.Type;
        if (type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segmentName}[{index}] pointe vers un objet sans type.");
        }

        if (type.IsArray)
        {
            return ResolveArrayElementNode(collection.AsArray(), type.ComponentType, index, $"{segmentName}[{index}]");
        }

        string typeName = type.Name ?? "";
        if (typeName.StartsWith("System.Collections.Generic.List<", StringComparison.Ordinal))
        {
            int size = SafeReadIntField(collection, "_size");
            if (index >= size)
            {
                throw new ClrSessionException($"Index hors limites pour {segmentName}[{index}] (taille logique {size}).");
            }

            ClrObject items = collection.ReadObjectField("_items");
            if (items.IsNull || items.Type is null || !items.Type.IsArray)
            {
                throw new ClrSessionException($"Stockage interne de la liste {segmentName} introuvable.");
            }
            return ResolveArrayElementNode(items.AsArray(), items.Type.ComponentType, index, $"{segmentName}[{index}]");
        }

        throw new ClrSessionException(
            $"Chemin CLR non supporte : {segmentName}[{index}] cible {typeName}. Seuls les tableaux et List<T> de references/structs sont supportes.");
    }

    /// <summary>
    /// Resout l'element d'un tableau CLR (index deja valide) en <see
    /// cref="PathNode"/> -- reference d'objet (comportement historique,
    /// <see cref="ClrArray.GetObjectValue"/>) ou struct (nouveau, chantier
    /// "ecriture indexee dans des tableaux de STRUCTS", <see
    /// cref="ClrArray.GetStructValue"/>). Un element STRING reste rejete ici
    /// (les strings n'ont pas de champ traversable) : seul un chemin qui se
    /// termine exactement sur <c>Champ[i]</c> (feuille) peut cibler une
    /// string, via <see cref="WriteIndexedReferenceValue"/> deja existant.
    /// </summary>
    private static PathNode ResolveArrayElementNode(ClrArray array, ClrType? componentType, int index, string label)
    {
        int length = array.GetLength(0);
        if (index >= length)
        {
            throw new ClrSessionException($"Index hors limites pour {label} (longueur {length}).");
        }
        if (componentType is null || componentType.ElementType == ClrElementType.String)
        {
            throw new ClrSessionException(
                $"Chemin CLR non supporte : {label} n'est pas une reference d'objet ou un struct traversable.");
        }

        if (componentType.IsValueType)
        {
            ClrValueType structValue = array.GetStructValue(index);
            if (!structValue.IsValid || structValue.Type is null)
            {
                throw new ClrSessionException($"Chemin CLR impossible : {label} (struct) invalide.");
            }
            return PathNode.FromValueType(structValue);
        }

        ClrObject item = array.GetObjectValue(index);
        if (item.IsNull || item.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {label} est null.");
        }
        return PathNode.FromObject(item);
    }

    private ClrRuntime RequireRuntime()
    {
        if (_runtime is null)
        {
            throw new ClrSessionException("Aucune session attachee -- appeler 'attach' avec un PID d'abord.");
        }
        return _runtime;
    }

    private static string ToHex(ulong address) => "0x" + address.ToString("x");

    private static ClrInstanceField? FindField(ClrType type, string fieldName)
    {
        ClrInstanceField? direct = type.GetFieldByName(fieldName);
        if (direct is not null) return direct;

        string backingName = $"<{fieldName}>k__BackingField";
        ClrInstanceField? backing = type.GetFieldByName(backingName);
        if (backing is not null) return backing;

        return type.Fields.FirstOrDefault(field => string.Equals(field.Name, fieldName, StringComparison.Ordinal));
    }

    private static ClrInstanceField? FindFirstField(ClrType type, params string[] fieldNames)
    {
        foreach (string fieldName in fieldNames)
        {
            ClrInstanceField? field = FindField(type, fieldName);
            if (field is not null)
            {
                return field;
            }
        }
        return null;
    }

    private readonly record struct PathSegment(string FieldName, int? Index, string? Key);

    private readonly record struct PathNode(ClrObject? Object, ClrValueType? ValueType)
    {
        public static PathNode FromObject(ClrObject obj) => new(obj, null);
        public static PathNode FromValueType(ClrValueType value) => new(null, value);
    }

    private static string FieldKind(ClrInstanceField field)
    {
        if (field.ElementType == ClrElementType.String) return "string";
        if (field.IsPrimitive) return "primitive";
        if (field.IsObjectReference) return "reference";
        return "value_type";
    }

    private static bool IsWritablePrimitive(ClrElementType elementType)
    {
        return elementType is ClrElementType.Boolean
            or ClrElementType.Int8
            or ClrElementType.UInt8
            or ClrElementType.Int16
            or ClrElementType.UInt16
            or ClrElementType.Int32
            or ClrElementType.UInt32
            or ClrElementType.Int64
            or ClrElementType.UInt64
            or ClrElementType.Float
            or ClrElementType.Double;
    }

    private static int PrimitiveSize(ClrElementType elementType)
    {
        return elementType switch
        {
            ClrElementType.Boolean or ClrElementType.Int8 or ClrElementType.UInt8 => 1,
            ClrElementType.Int16 or ClrElementType.UInt16 => 2,
            ClrElementType.Int32 or ClrElementType.UInt32 or ClrElementType.Float => 4,
            ClrElementType.Int64 or ClrElementType.UInt64 or ClrElementType.Double => 8,
            _ => 0,
        };
    }

    private static ulong ParseReferenceValue(string valueText)
    {
        string value = valueText.Trim();
        if (string.Equals(value, "null", StringComparison.OrdinalIgnoreCase) || value == "0")
        {
            return 0;
        }
        return ParseHexAddress(value);
    }

    private static byte[] EncodePointer(ulong address)
    {
        return IntPtr.Size == 8
            ? BitConverter.GetBytes(address)
            : BitConverter.GetBytes(checked((uint)address));
    }

    private static byte[] EncodePrimitive(ClrElementType elementType, string valueText)
    {
        string value = valueText.Trim();
        return elementType switch
        {
            ClrElementType.Boolean => new[] { ParseBoolean(value) ? (byte)1 : (byte)0 },
            ClrElementType.Int8 => new[] { unchecked((byte)sbyte.Parse(value, CultureInfo.InvariantCulture)) },
            ClrElementType.UInt8 => new[] { byte.Parse(value, CultureInfo.InvariantCulture) },
            ClrElementType.Int16 => BitConverter.GetBytes(short.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.UInt16 => BitConverter.GetBytes(ushort.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.Int32 => BitConverter.GetBytes(int.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.UInt32 => BitConverter.GetBytes(uint.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.Int64 => BitConverter.GetBytes(long.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.UInt64 => BitConverter.GetBytes(ulong.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.Float => BitConverter.GetBytes(float.Parse(value, CultureInfo.InvariantCulture)),
            ClrElementType.Double => BitConverter.GetBytes(double.Parse(value, CultureInfo.InvariantCulture)),
            _ => throw new ClrSessionException($"Type primitif non supporte en ecriture : {elementType}"),
        };
    }

    private static bool ParseBoolean(string value)
    {
        if (bool.TryParse(value, out bool parsed)) return parsed;
        if (value == "1") return true;
        if (value == "0") return false;
        throw new FormatException($"Booleen invalide : {value}");
    }

    private static bool PrimitiveMatches(ClrElementType elementType, string expectedText, object? actual)
    {
        if (actual is null) return false;
        try
        {
            object expected = elementType switch
            {
                ClrElementType.Boolean => ParseBoolean(expectedText.Trim()),
                ClrElementType.Int8 => sbyte.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.UInt8 => byte.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.Int16 => short.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.UInt16 => ushort.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.Int32 => int.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.UInt32 => uint.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.Int64 => long.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.UInt64 => ulong.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.Float => float.Parse(expectedText, CultureInfo.InvariantCulture),
                ClrElementType.Double => double.Parse(expectedText, CultureInfo.InvariantCulture),
                _ => expectedText,
            };
            return expected.Equals(actual);
        }
        catch
        {
            return false;
        }
    }

    private static bool FieldValueMatches(ClrElementType elementType, object? actual, string expectedText)
    {
        if (actual is null) return string.Equals(expectedText.Trim(), "null", StringComparison.OrdinalIgnoreCase);
        if (elementType == ClrElementType.String)
        {
            return string.Equals(Convert.ToString(actual, CultureInfo.InvariantCulture), expectedText, StringComparison.Ordinal);
        }
        if (IsPrimitiveElement(elementType) && elementType != ClrElementType.Char)
        {
            return PrimitiveMatches(elementType, expectedText, actual);
        }
        return string.Equals(Convert.ToString(actual, CultureInfo.InvariantCulture), expectedText, StringComparison.Ordinal);
    }

    private static ulong ParseHexAddress(string addressHex)
    {
        string trimmed = addressHex.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? addressHex[2..] : addressHex;
        if (!ulong.TryParse(trimmed, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out ulong value))
        {
            throw new ClrSessionException($"Adresse hexadecimale invalide : {addressHex}");
        }
        return value;
    }

    public void Dispose() => DetachInternal();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeProcessHandle OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(
        SafeProcessHandle process,
        IntPtr baseAddress,
        byte[] buffer,
        int size,
        out nint bytesWritten);
}

public sealed class SafeProcessHandle : SafeHandle
{
    public SafeProcessHandle() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero || handle == new IntPtr(-1);

    protected override bool ReleaseHandle() => CloseHandle(handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}

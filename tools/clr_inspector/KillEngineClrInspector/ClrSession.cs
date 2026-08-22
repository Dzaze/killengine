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

    private static object? ReadFieldValue(ClrObject obj, ClrInstanceField field)
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
            return DescribeObjectReference(refObj, depth: 0);
        }

        // Champ value-type (struct) autre que primitif : hors perimetre MVP
        // (ex: decimal, struct utilisateur imbriquee) -- volontairement pas
        // decode ici, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md "hors scope MVP".
        return $"<value-type non deroule: {field.Type?.Name ?? field.ElementType.ToString()}>";
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
        return $"<value-type non deroule: {field.Type?.Name ?? field.ElementType.ToString()}>";
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

    // Types de parametre primitif supportes pour l'appel reel d'un setter
    // (v1) -- exactement le meme perimetre entier que IsWritablePrimitive
    // (bool + entiers 8/16/32/64 signes/non signes), MOINS Float/Double :
    // la convention d'appel x64 Windows passe un 2e argument flottant en
    // XMM1, pas RDX -- shellcode fixe (mov rdx, imm64) volontairement hors
    // scope pour ce cas, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md.
    // Noms exacts tels que rapportes par ClrMethod.Signature -- verifies par
    // attache ClrMD reelle sur ce process de test (pas devines), reflexion
    // ponctuelle avant d'ecrire cette methode : Boolean/SByte/Byte/Int16/
    // UInt16/Int32/UInt32/Int64/UInt64 pour les primitifs entiers (noms
    // courts CLR, sans namespace) ; un type non primitif (string, objet,
    // struct) apparait toujours prefixe de son namespace complet
    // (ex: "System.String", "KillEngine.ClrTestTarget.Item"), donc jamais en
    // collision avec cette liste.
    private static readonly HashSet<string> SupportedInstanceMethodParameterTypes = new(StringComparer.Ordinal)
    {
        "Boolean", "SByte", "Byte", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64",
    };

    /// <summary>
    /// Resout l'adresse native deja JITtee d'un setter d'instance reel (pas
    /// static, 0 ou 1 parametre primitif entier) pour permettre a KillEngine
    /// (cote natif, injection shellcode) de l'appeler directement -- ClrMD
    /// est une API de lecture passive (DAC), elle n'execute jamais de code
    /// cible elle-meme, cette methode se limite donc a la RESOLUTION
    /// d'adresse. Voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour le detail
    /// complet du perimetre v1 et son mecanisme d'appel (shellcode x64 cote
    /// ApplicationController::callClrInstanceMethod).
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
        if (parameterType is not null && !SupportedInstanceMethodParameterTypes.Contains(parameterType))
        {
            throw new ClrSessionException(
                $"Type de parametre non supporte : {obj.Type.Name}.{resolvedName}({parameterType}). " +
                "Seuls bool/int8/int16/int32/int64 (signes et non signes) sont geres en v1 -- " +
                "pas float/double (convention d'appel XMM1, hors scope), pas string/objet/struct.");
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
        if (arrayObject.Type?.ComponentType is null || arrayObject.Type.ComponentType.IsValueType || arrayObject.Type.ComponentType.ElementType == ClrElementType.String)
        {
            throw new ClrSessionException($"Element {segment.FieldName}[{index}] non supporte : seules les references objet sont modifiables ici.");
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
                : PathNode.FromObject(ResolveIndexedReference(next, segment.Index.Value, segment.FieldName));
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
                : PathNode.FromObject(ResolveIndexedReference(next, segment.Index.Value, segment.FieldName));
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

    private static ClrObject ResolveIndexedReference(ClrObject collection, int index, string segmentName)
    {
        ClrType? type = collection.Type;
        if (type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {segmentName}[{index}] pointe vers un objet sans type.");
        }

        if (type.IsArray)
        {
            return ResolveArrayReferenceElement(collection.AsArray(), type.ComponentType, index, $"{segmentName}[{index}]");
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
            return ResolveArrayReferenceElement(items.AsArray(), items.Type.ComponentType, index, $"{segmentName}[{index}]");
        }

        throw new ClrSessionException(
            $"Chemin CLR non supporte : {segmentName}[{index}] cible {typeName}. Seuls les tableaux et List<T> de references sont supportes.");
    }

    private static ClrObject ResolveArrayReferenceElement(ClrArray array, ClrType? componentType, int index, string label)
    {
        int length = array.GetLength(0);
        if (index >= length)
        {
            throw new ClrSessionException($"Index hors limites pour {label} (longueur {length}).");
        }
        if (componentType is null || componentType.ElementType == ClrElementType.String || componentType.IsValueType)
        {
            throw new ClrSessionException(
                $"Chemin CLR non supporte : {label} n'est pas une reference d'objet mutable avec champs.");
        }

        ClrObject item = array.GetObjectValue(index);
        if (item.IsNull || item.Type is null)
        {
            throw new ClrSessionException($"Chemin CLR impossible : {label} est null.");
        }
        return item;
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

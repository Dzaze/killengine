using Microsoft.Diagnostics.Runtime;

namespace KillEngine.ClrInspector;

public sealed class ClrSessionException : Exception
{
    public ClrSessionException(string message) : base(message) { }
}

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

    private DataTarget? _dataTarget;
    private ClrRuntime? _runtime;

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
            return new { address = ToHex(obj.Address), typeName = (string?)null, fields = new Dictionary<string, object?>() };
        }

        var fields = new Dictionary<string, object?>();
        foreach (ClrInstanceField field in type.Fields)
        {
            string name = field.Name ?? "?";
            try
            {
                fields[name] = ReadFieldValue(obj, field);
            }
            catch (Exception ex)
            {
                fields[name] = $"<erreur lecture: {ex.Message}>";
            }
        }

        return new
        {
            address = ToHex(obj.Address),
            typeName = type.Name,
            size = obj.Size,
            fields,
            collection = DescribeCollection(obj, depth: 0),
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
        }
        catch (Exception ex)
        {
            return new { kind = "collection_error", error = ex.Message };
        }

        return null;
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

    private ClrRuntime RequireRuntime()
    {
        if (_runtime is null)
        {
            throw new ClrSessionException("Aucune session attachee -- appeler 'attach' avec un PID d'abord.");
        }
        return _runtime;
    }

    private static string ToHex(ulong address) => "0x" + address.ToString("x");

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
}

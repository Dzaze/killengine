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
            return new
            {
                address = ToHex(refObj.Address),
                typeName = refObj.Type?.Name,
            };
        }

        // Champ value-type (struct) autre que primitif : hors perimetre MVP
        // (ex: decimal, struct utilisateur imbriquee) -- volontairement pas
        // decode ici, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md "hors scope MVP".
        return $"<value-type non deroule: {field.Type?.Name ?? field.ElementType.ToString()}>";
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

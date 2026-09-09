# Shared by the live native census and its controlled negative fixtures.
# This file defines validation only; it cannot launch an editor or update the
# reviewed manifest. Registration names are case-sensitive identifiers.

function ConvertFrom-RegistrationCensusRecord($Record, [string]$Source) {
    if ($Record -isnot [System.Management.Automation.PSCustomObject]) {
        throw "$Source census must be a JSON object"
    }
    $categories = @('world', 'dsl', 'property', 'editor')
    foreach ($entry in $Record.PSObject.Properties) {
        if ($entry.Name -cnotin $categories) {
            throw "$Source census has unexpected category '$($entry.Name)'"
        }
    }
    $result = [ordered]@{}
    foreach ($category in $categories) {
        $entry = $Record.PSObject.Properties[$category]
        if ($null -eq $entry) { throw "$Source census omitted '$category'" }
        if ($entry.Value -isnot [System.Array] -or $entry.Value.Count -eq 0) {
            throw "$Source census requires a nonempty array for '$category'"
        }
        $unique = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($value in $entry.Value) {
            if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
                throw "$Source census contains an invalid '$category' registration"
            }
            if (-not $unique.Add($value)) {
                throw "$Source census reported duplicate '$category' registrations"
            }
        }
        [string[]]$names = @($entry.Value)
        [System.Array]::Sort($names, [System.StringComparer]::Ordinal)
        $result[$category] = $names
    }
    return $result
}

function ConvertFrom-RuntimeRegistrationCensusLog([string]$Text, [string]$Source) {
    $prefix = 'MATTER_REGISTRATION_CENSUS_JSON='
    $records = @($Text -split "`r?`n" |
        Where-Object { $_.StartsWith($prefix, [System.StringComparison]::Ordinal) })
    if ($records.Count -ne 1) {
        throw "$Source emitted $($records.Count) runtime census records; expected exactly one"
    }
    try {
        $record = $records[0].Substring($prefix.Length) | ConvertFrom-Json
    } catch {
        throw "$Source emitted invalid runtime census JSON: $($records[0])"
    }
    return ConvertFrom-RegistrationCensusRecord $record $Source
}

function Assert-RegistrationCensusMatches($Expected, $Actual, [string]$ActualName) {
    foreach ($category in @('world', 'dsl', 'property', 'editor')) {
        $missing = @($Expected[$category] |
            Where-Object { $_ -cnotin $Actual[$category] })
        $extra = @($Actual[$category] |
            Where-Object { $_ -cnotin $Expected[$category] })
        if ($missing.Count -eq 0 -and $extra.Count -eq 0) { continue }

        $message = "$category registration mismatch: reviewed manifest vs $ActualName"
        if ($missing.Count -gt 0) {
            $message += "`n  missing from ${ActualName}: $($missing -join ', ')"
        }
        if ($extra.Count -gt 0) {
            $message += "`n  extra in ${ActualName}: $($extra -join ', ')"
        }
        throw $message
    }
}

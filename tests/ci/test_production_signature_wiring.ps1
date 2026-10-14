param(
    [string]$Collector = (Join-Path $PSScriptRoot "collect_production_evidence.ps1")
)

$ErrorActionPreference = "Stop"
$source = Get-Content -LiteralPath $Collector -Raw
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw "Collector syntax is invalid" }

function Assignment([string]$Name) {
    $found = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $node.Operator -eq [System.Management.Automation.Language.TokenKind]::Equals -and
        $node.Left.Extent.Text -eq ('$' + $Name)
    }, $true))
    if ($found.Count -ne 1) { throw "Expected one collector assignment for $Name" }
    return $found[0].Extent.Text
}

$relativePath = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-RelativePath'
}, $true))
if ($relativePath.Count -ne 1) { throw "Collector relative-path function is missing" }
$scan = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.ForEachStatementAst] -and
    $node.Condition.Extent.Text -eq '$signatureRules'
}, $true))
if ($scan.Count -ne 1) { throw "Expected the actual collector signature scan" }

# Execute the production assignments and scan, with only the process results
# and logs supplied by the fixture. This invokes no Godot process or GPU job.
$program = @(
    $relativePath[0].Extent.Text,
    (Assignment 'RuntimeSkipMarkerPattern'),
    (Assignment 'signatureRules'),
    (Assignment 'signatureHits'),
    $scan[0].Extent.Text,
    (Assignment 'loopResults'),
    (Assignment 'loopFailCount'),
    (Assignment 'loopLeakHits'),
    (Assignment 'issue900Ready')
) -join "`n"
$captured = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixtures/production_leak_warning.json') -Raw | ConvertFrom-Json
$runDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $runDir | Out-Null

function Check([string]$Name, [string]$Text, [bool]$InLoop, [bool]$Passed, [bool]$ExpectedReady, [int]$ExpectedLeakHits, [int]$ExpectedInvariantHits) {
    $file = Join-Path $runDir $(if ($InLoop) { 'runtime_loops_case.log' } else { 'qa_case.log' })
    Set-Content -LiteralPath $file -Value $Text
    $allLogFiles = @(Get-Item -LiteralPath $file)
    $results = @([pscustomobject]@{ name = 'loop1_runtime_control'; passed = $Passed })
    Invoke-Expression $program
    $leaks = @($signatureHits | Where-Object { $_.rule -eq 'leak' }).Count
    $invariants = @($signatureHits | Where-Object { $_.rule -eq 'streaming_invariant' }).Count
    if ($issue900Ready -ne $ExpectedReady -or $leaks -ne $ExpectedLeakHits -or $invariants -ne $ExpectedInvariantHits) {
        throw "$Name failed: 900_ready=$issue900Ready leak_hits=$leaks invariant_hits=$invariants"
    }
}

try {
    Check 'healthy loop' 'Completed without allocation errors.' $true $true $true 0 0
    Check 'captured leaked RID in loop blocks 900' $captured.line $true $true $false 1 0
    Check 'leak outside loops is reported but not attributed to 900' $captured.line $false $true $true 1 0
    Check 'word boundaries avoid substring false positives' 'bleak outlook; leakproof storage' $true $true $true 0 0
    Check 'failed command blocks 900 without a leak' 'Completed without allocation errors.' $true $false $false 0 0
    # A constructed malformed-state token, not a claimed runtime capture.
    # This rule reports the token; it is not an additional 900 readiness input.
    Check 'streaming invariant token is recorded' '[Streaming] Invalid chunk state' $true $true $true 0 1
    # Prove that restoring either concrete defect makes the production-path
    # assertion fail. These are test-local mutations, never collector writes.
    $correctProgram = $program
    $mutations = @(
        @{ name = 'overescaped leak pattern'; text = $program.Replace('"\bleak(ed)?\b"', '"\\bleak(ed)?\\b"') },
        @{ name = 'leak disconnected from readiness'; text = $program.Replace('$loopLeakHits.Count -eq 0', '$true') },
        @{ name = 'overescaped invariant pattern'; text = $program.Replace('"\[Streaming\] Invalid chunk state"', '"\\[Streaming\\] Invalid chunk state"') }
    )
    foreach ($mutation in $mutations) {
        if ($mutation.text -eq $correctProgram) { throw "Mutation did not alter the production program: $($mutation.name)" }
        $program = $mutation.text
        $failed = $false
        try {
            if ($mutation.name -eq 'overescaped invariant pattern') {
                Check 'mutated invariant detection' '[Streaming] Invalid chunk state' $true $true $true 0 1
            } else {
                Check 'mutated captured leak readiness' $captured.line $true $true $false 1 0
            }
        } catch { $failed = $true }
        if (-not $failed) { throw "Regression assertion survived mutation: $($mutation.name)" }
    }
    $program = $correctProgram
    Write-Output 'Production signature scan and readiness wiring: 6 cases passed.'
    Write-Output 'Production signature regression controls: 3 mutations rejected.'
} finally {
    Remove-Item -LiteralPath $runDir -Recurse -Force
}

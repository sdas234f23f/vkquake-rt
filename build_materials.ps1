$s = "$PWD\vkpt\Source"; $g = "$PWD\build\Debug\id1"
New-Item -ItemType Directory -Path "$g\materials" -Force | Out-Null
Copy-Item "$s\materials.yaml" "$g\materials\materials.yaml" -Force
foreach ($sub in @("textures","progs")) { if (Test-Path "$s\$sub") { New-Item -ItemType Directory -Path "$g\$sub" -Force | Out-Null; Copy-Item -Path "$s\$sub\*" -Destination "$g\$sub" -Recurse -Force } }
Write-Host "Deployed materials into $g" -ForegroundColor Green
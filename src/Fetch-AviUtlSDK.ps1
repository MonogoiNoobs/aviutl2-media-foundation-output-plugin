<#
	SPDX-License-Identifier: MIT
	Copyright (c) 2025-2026 Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
#>

If (Test-Path aviutl2_sdk)
{
	Remove-Item aviutl2_sdk -Recurse
}
Invoke-WebRequest https://spring-fragrance.mints.ne.jp/aviutl/aviutl2_sdk.zip -OutFile aviutl2_sdk.zip
Expand-Archive aviutl2_sdk.zip
Remove-Item aviutl2_sdk.zip
Remove-Item aviutl2_sdk\* -Exclude *.h

Get-ChildItem aviutl2_sdk\* | % { Get-Content $_ -Encoding OEM | Out-File "$_.utf8" }
Remove-Item aviutl2_sdk\* -Exclude *.utf8
Get-ChildItem aviutl2_sdk\*.utf8 | % { Rename-Item $_ ($_ -Replace ".utf8$","") }

Write-Output "Fetched the latest AviUtl ExEdit2 Plugin SDK."
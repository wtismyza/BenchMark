<#
  Copyright (c) 2014-present, The osquery authors

  This source code is licensed as defined by the LICENSE file found in the
  root directory of this source tree.

  SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
#>

#
# Download and extract Strawberry Perl
#

# It does not seem that they have HTTPS: https://github.com/StrawberryPerl/strawberryperl.com/issues/11
(New-Object System.Net.WebClient).DownloadFile("http://strawberryperl.com/download/5.30.0.1/strawberry-perl-5.30.0.1-64bit.zip", "$env:TEMP\strawberry_perl.zip")

# Prefer 7zip if present, which is faster
if (Get-Command 7z -errorAction SilentlyContinue)
{
	7z x -oC:\Strawberry -y "$env:TEMP\strawberry_perl.zip"
}
else
{
	Write-Host "Couldn't find 7zip, will use the slower Expand-Archive"
	Expand-Archive -Path "$env:TEMP\strawberry_perl.zip" -DestinationPath "C:\Strawberry" -Force
}

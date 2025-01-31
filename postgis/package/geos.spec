Summary:        Geos library
License:        LGPL
Name:           geos
Version:        %{geos_ver}
Release:        %{geos_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
Provides:       geos = %{geos_ver}

%define _build_id_links none

%description
The Geos module provides geometric library which is used by PostGIS.

%install
mkdir -p %{buildroot}/temp/lib
cp -rf %{geos_dir}/lib64/libgeos*so* %{buildroot}/temp/lib/

%files
/temp

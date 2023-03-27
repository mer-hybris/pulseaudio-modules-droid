%define pulseversion %{expand:%(rpm -q --qf '[%%{version}]' pulseaudio)}
%define pulsemajorminor %{expand:%(echo '%{pulseversion}' | cut -d+ -f1)}
%define moduleversion %{pulsemajorminor}.%{expand:%(echo '%{version}' | cut -d. -f3)}

Name:       pulseaudio-modules-droid

Summary:    PulseAudio Droid HAL modules
Version:    %{pulsemajorminor}.101
Release:    1
License:    LGPLv2+
URL:        https://github.com/mer-hybris/pulseaudio-modules-droid
Source0:    %{name}-%{version}.tar.bz2
Requires:   pulseaudio >= %{pulseversion}
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio-module-keepalive >= 1.0.0
BuildRequires:  libtool-ltdl-devel
BuildRequires:  meson
BuildRequires:  pkgconfig(pulsecore) >= %{pulsemajorminor}
BuildRequires:  pkgconfig(android-headers)
BuildRequires:  pkgconfig(libhardware)
BuildRequires:  pkgconfig(expat)

%description
PulseAudio Droid HAL modules.

%package common
Summary:    Common libs for the PulseAudio droid modules
Requires:   pulseaudio >= %{pulseversion}

%description common
This contains common libs for the PulseAudio droid modules.

%package devel
Summary:    Development files for PulseAudio droid modules
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description devel
This contains development files for PulseAudio droid modules.

%prep
%autosetup -n %{name}-%{version}

%build
echo "%{moduleversion}" > .tarball-version
# Obtain the DEVICE from the same source as used in /etc/os-release
if [ -e "%{_includedir}/droid-devel/hw-release.vars" ]; then
. %{_includedir}/droid-devel/hw-release.vars
else
. %{_libdir}/droid-devel/hw-release.vars
fi

%meson -Ddroid-device=$MER_HA_DEVICE
%meson_build

%install
%meson_install

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-sink.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-source.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-sink.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-source.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-card.so
%license COPYING

%files common
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-util.so

%files devel
%defattr(-,root,root,-)
%dir %{_includedir}/pulsecore/modules/droid
%{_includedir}/pulsecore/modules/droid/conversion.h
%{_includedir}/pulsecore/modules/droid/droid-config.h
%{_includedir}/pulsecore/modules/droid/droid-util.h
%{_includedir}/pulsecore/modules/droid/sllist.h
%{_includedir}/pulsecore/modules/droid/utils.h
%{_includedir}/pulsecore/modules/droid/version.h
%{_libdir}/pkgconfig/*.pc

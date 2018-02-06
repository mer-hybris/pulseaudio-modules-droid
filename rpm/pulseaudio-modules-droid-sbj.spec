%define device sbj
%define pulseversion %{expand:%(rpm -q --qf '[%%{version}]' pulseaudio)}
%define pulsemajorminor %{expand:%(echo '%{pulseversion}' | cut -d+ -f1)}
%define moduleversion %{pulsemajorminor}.%{expand:%(echo '%{version}' | cut -d. -f3)}

Name:       pulseaudio-modules-droid-%{device}

Summary:    PulseAudio Droid HAL modules
Version:    %{pulsemajorminor}.65
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
URL:        https://github.com/mer-hybris/pulseaudio-modules-droid
Source0:    %{name}-%{version}.tar.bz2
Requires:   pulseaudio >= %{pulseversion}
Requires:   %{name}-common = %{version}-%{release}
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  libtool-ltdl-devel
BuildRequires:  pkgconfig(pulsecore) >= %{pulsemajorminor}
BuildRequires:  pkgconfig(android-headers)
BuildRequires:  pkgconfig(libhardware)
BuildRequires:  pkgconfig(dbus-1)
Provides: pulseaudio-modules-droid

%description
PulseAudio Droid HAL modules.

%package common
Summary:    Common libs for the PulseAudio droid modules
Group:      Multimedia/PulseAudio
Requires:   pulseaudio >= %{pulseversion}

%description common
This contains common libs for the PulseAudio droid modules.

%package devel
Summary:    Development files for PulseAudio droid modules
Group:      Development/Libraries
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description devel
This contains development files for PulseAudio droid modules.

%prep
%setup -q -n %{name}-%{version}

%build
echo "%{moduleversion}" > .tarball-version
%reconfigure --disable-static --with-droid-device=%{device}
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

install -d %{buildroot}/%{_prefix}/include/pulsecore/modules/droid
install -m 644 src/common/*.h %{buildroot}/%{_prefix}/include/pulsecore/modules/droid
install -d %{buildroot}/%{_libdir}/pkgconfig
install -m 644 src/common/*.pc %{buildroot}/%{_libdir}/pkgconfig

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-sink.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-source.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-sink.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-source.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-card.so
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-droid-keepalive.so

%files common
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/libdroid-util.so

%files devel
%defattr(-,root,root,-)
%{_prefix}/include/pulsecore/modules/droid/*.h
%{_libdir}/pkgconfig/*.pc

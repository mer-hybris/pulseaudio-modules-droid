%define device sbj
%define pulseversion %{expand:%(rpm -q --qf '[%%{version}]' pulseaudio)}
%define pulsemajorminor %{expand:%(echo '%{pulseversion}' | cut -d+ -f1)}
%define moduleversion %{pulsemajorminor}.%{expand:%(echo '%{version}' | cut -d. -f3)}

Name:       pulseaudio-modules-droid-%{device}

Summary:    PulseAudio Droid HAL modules
Version:    %{pulsemajorminor}.1
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
URL:        https://github.com/mer-hybris/multimedia-pulseaudio-modules-droid
Source0:    %{name}-%{version}.tar.bz2
Requires:   pulseaudio >= %{pulseversion}
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


%prep
%setup -q -n %{name}-%{version}

%build
echo "%{moduleversion}" > .tarball-version
%reconfigure --disable-static --with-droid-device=%{device}
make %{?jobs:-j%jobs}

%pre
systemctl-user stop pulseaudio.service || :

%post
systemctl-user daemon-reload || :
systemctl-user restart pulseaudio.service || :

%install
rm -rf %{buildroot}
%make_install

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/*.so

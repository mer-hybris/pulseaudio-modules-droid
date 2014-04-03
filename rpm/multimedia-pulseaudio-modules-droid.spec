%define pulseversion 4.0

Name:       pulseaudio-modules-droid

Summary:    PulseAudio Droid HAL modules
Version:    %{pulseversion}.1
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
URL:        https://github.com/mer-hybris/multimedia-pulseaudio-modules-droid
Source0:    %{name}-%{version}.tar.bz2
Source1:    pulseaudio-modules-droid.spec.in
Source2:    precheckin.sh
Requires:   pulseaudio >= %{pulseversion}
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  libtool-ltdl-devel
BuildRequires:  pkgconfig(pulsecore) >= %{pulseversion}
BuildRequires:  pkgconfig(android-headers)
BuildRequires:  pkgconfig(libhardware)
BuildRequires:  pkgconfig(dbus-1)

%description
PulseAudio Droid HAL modules.


%prep
%setup -q -n %{name}-%{version}

%build
# Obtain the DEVICE from the hw-release info
. /etc/os-release.d/hw-release
%reconfigure --disable-static --with-droid-device=$MER_HA_DEVICE
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulseversion}/modules/*.so

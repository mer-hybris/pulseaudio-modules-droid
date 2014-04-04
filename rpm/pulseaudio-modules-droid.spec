%define pulseversion 4.0

Name:       pulseaudio-modules-droid

Summary:    PulseAudio Droid HAL modules
Version:    %{pulseversion}.1
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
URL:        https://github.com/mer-hybris/pulseaudio-modules-droid
Source0:    %{name}-%{version}.tar.bz2
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
# Obtain the DEVICE from the same source as used in /etc/os-release
. /usr/lib/droid-devel/hw-release.vars
%reconfigure --disable-static --with-droid-device=$MER_HA_DEVICE
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulseversion}/modules/*.so

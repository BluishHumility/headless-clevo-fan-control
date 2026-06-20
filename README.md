# Clevo Fan Control on Linux

A service for controlling the fans on Clevo laptops.

## Build and Install

Run the following from a directory where you are
comfortable having temporary build files generated.

By default the service will be installed to `/usr/local/bin` and the
service file for autostart to `/etc/systemd/system/clevo-fan-control.service`.

```shell
cmake --build .
sudo cmake --install .
systemctl enable --now clevo-fan-control
```

### Uninstalling

Use the `install_manifest.txt` generated when installing to remove the
files.

```shell
sudo xargs rm < install_manifest.txt
```

## Credits and Acknowledgements

This project is based on https://github.com/agramian/clevo-fan-control

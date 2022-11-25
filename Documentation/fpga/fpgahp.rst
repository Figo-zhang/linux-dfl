.. SPDX-License-Identifier: GPL-2.0

===========================
FPGA Hotplug Manager Driver
===========================

Authors:

- Tianfei Zhang <tianfei.zhang@intel.com>

There are some board managements for PCIe-based FPGA card like burning the entire
image, loading a new FPGA image or BMC firmware in FPGA deployment of data center
or cloud. For example, loading a new FPGA image, the driver needs to remove all of
PCI devices like PFs/VFs and as well as any other types of devices (platform, etc.)
defined within the FPGA. After triggering the image load of the FPGA card via BMC,
the driver reconfigures the PCI bus. The FPGA Hotplug Manager (fpgahp) driver manages
those devices and functions leveraging the PCI hotplug framework to deal with the
reconfiguration of the PCI bus and removal/probe of PCI devices below the FPGA card.

This fpgahp driver adds 2 new callbacks to extend the hotplug mechanism to
allow selecting and loading a new FPGA image.

 - available_images: Optional: called to return the available images of a FPGA card.
 - image_load: Optional: called to load a new image for a FPGA card.

In general, the fpgahp driver provides some sysfs files::

        /sys/bus/pci/slots/<X-X>/available_images
        /sys/bus/pci/slots/<X-X>/image_load

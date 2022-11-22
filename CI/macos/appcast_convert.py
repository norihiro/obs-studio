import os

import xmltodict


DELTA_BASE_URL = 'https://cdn-fastly.obsproject.com/downloads/sparkle_deltas'


def convert_appcast(filename):
    print('Converting', filename)
    in_path = os.path.join('output/appcasts', filename)
    out_path = os.path.join('output/appcasts', filename.replace('_v2', ''))
    with open(in_path, 'rb') as f:
        xml_data = f.read()
    if not xml_data:
        return

    appcast = xmltodict.parse(xml_data, force_list=('item',))

    new_list = []
    # Remove anything but the first stable channel item.
    # Mostly due to some issues with older Sparkle versions
    # we still have out there.
    for item in appcast['rss']['channel']['item']:
        branch = item.pop('sparkle:channel', 'stable')
        if branch != 'stable':
            continue
        # remove delta information (incompatible with Sparkle 1.x)
        item.pop('sparkle:deltas', None)
        new_list.append(item)
        break

    appcast['rss']['channel']['item'] = new_list

    with open(out_path, 'wb') as f:
        xmltodict.unparse(appcast, output=f, pretty=True)


def adjust_appcast(filename):
    print('Adjusting', filename)
    file_path = os.path.join('output/appcasts', filename)
    with open(file_path, 'rb') as f:
        xml_data = f.read()
    if not xml_data:
        return

    arch = 'arm64' if 'arm64' in filename else 'x86_64'
    appcast = xmltodict.parse(xml_data, force_list=('item', 'enclosure'))
    appcast['rss']['channel']['title'] = 'OBS Studio'
    appcast['rss']['channel']['link'] = 'https://obsproject.com/'

    new_list = []
    for item in appcast['rss']['channel']['item']:
        # Fix changelog URL
        # Sparkle 2.x *really* wants us to embed the release notes instead of specifying
        # a URL, and will not display the full notes, so use the legacy attribute instead.
        if release_notes_link := item.pop('sparkle:fullReleaseNotesLink', None):
            item['sparkle:releaseNotesLink'] = release_notes_link

        # If deltas exist, update their URLs to match server layout
        # (generate_appcast doesn't allow this).
        if deltas := item.pop('sparkle:deltas', None):
            for delta_item in deltas['enclosure']:
                delta_filename = delta_item['@url'].rpartition('/')[2]
                delta_item['@url'] = f'{DELTA_BASE_URL}/{arch}/{delta_filename}'

            item['sparkle:deltas'] = deltas

        new_list.append(item)

    appcast['rss']['channel']['item'] = new_list

    with open(file_path, 'wb') as f:
        xmltodict.unparse(appcast, output=f, pretty=True)


if __name__ == '__main__':
    for ac_file in os.listdir('output/appcasts'):
        if 'v2' not in ac_file:
            continue
        adjust_appcast(ac_file)
        convert_appcast(ac_file)

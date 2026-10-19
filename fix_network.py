with open('/home/deck/minecraft-javac/src/network.c', 'r') as f:
    content = f.read()

old = '''        float fyaw = p->yaw, fpitch = p->pitch;
        memcpy(&ux, &dx, 8); memcpy(&uy, &dy, 8); memcpy(&uz, &dz, 8);
        memcpy(&uyaw, &fyaw, 4); memcpy(&upitch, &fpitch, 4);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((ux >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uy >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uz >> (8 * (7 - i))) & 0xFF);
        buf[n++] = p->on_ground ? 1 : 0;   // onGround'''

new = '''        double dx = p->x, dy = p->y, dz = p->z;
        memcpy(&ux, &dx, 8); memcpy(&uy, &dy, 8); memcpy(&uz, &dz, 8);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((ux >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uy >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uz >> (8 * (7 - i))) & 0xFF);
        buf[n++] = p->on_ground ? 1 : 0;   // onGround'''

if old in content:
    content = content.replace(old, new, 1)
    with open('/home/deck/minecraft-javac/src/network.c', 'w') as f:
        f.write(content)
    print('Replaced successfully')
else:
    print('Pattern not found!')
    # Show what we have around that area
    lines = content.split('\n')
    for i in range(1320, 1345):
        print(f'{i}: {lines[i]}')
#ifndef KERNEL_PIC_H
#define KERNEL_PIC_H

void pic_remap(void);
void pic_send_eoi(unsigned char irq);
void pic_set_mask(unsigned char irq_line);
void pic_clear_mask(unsigned char irq_line);

#endif

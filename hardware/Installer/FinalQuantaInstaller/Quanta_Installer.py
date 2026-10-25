import os, sys, subprocess, threading, queue, re
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from serial.tools import list_ports

BASE=Path(__file__).resolve().parent
class Installer:
 def __init__(self, root):
  self.root=root; root.title('Quanta Firmware Installer');root.geometry('700x520')
  self.port=tk.StringVar();self.firmware=tk.StringVar(value=str(BASE/'firmware.bin'))
  self.status=tk.StringVar(value='Ready. Connect ESP-PROG-2 and choose a COM port.')
  self.q=queue.Queue();self.busy=False
  f=ttk.Frame(root,padding=16);f.pack(fill='both',expand=True)
  ttk.Label(f,text='Quanta Firmware Installer',font=('Segoe UI',17,'bold')).pack(anchor='w')
  ttk.Label(f,text='ESP32-WROOM-32E-N8 • 8 MB flash • ESP-PROG-2').pack(anchor='w',pady=(0,15))
  p=ttk.Frame(f);p.pack(fill='x');ttk.Label(p,text='Serial port:').pack(side='left');self.ports=ttk.Combobox(p,textvariable=self.port,width=46,state='readonly');self.ports.pack(side='left',padx=8);ttk.Button(p,text='Refresh',command=self.refresh).pack(side='left')
  x=ttk.LabelFrame(f,text='Firmware file (for updates)',padding=10);x.pack(fill='x',pady=12)
  ttk.Entry(x,textvariable=self.firmware).pack(side='left',fill='x',expand=True)
  ttk.Button(x,text='Browse…',command=self.browse).pack(side='left',padx=(8,0))
  b=ttk.Frame(f);b.pack(fill='x',pady=5)
  self.factory=ttk.Button(b,text='FACTORY INSTALL (ERASE ALL)',command=lambda:self.start('factory'));self.factory.pack(side='left',padx=(0,10))
  self.update=ttk.Button(b,text='UPDATE FIRMWARE (KEEP SETTINGS)',command=lambda:self.start('update'));self.update.pack(side='left')
  ttk.Label(f,text='Factory install erases Wi-Fi, identity, and all stored data. Update preserves NVS and SPIFFS.',wraplength=650).pack(anchor='w',pady=(8,8))
  self.progress=ttk.Progressbar(f,mode='indeterminate');self.progress.pack(fill='x',pady=(0,5))
  ttk.Label(f,textvariable=self.status,wraplength=650).pack(anchor='w')
  self.log=tk.Text(f,height=13,wrap='word',state='disabled');self.log.pack(fill='both',expand=True,pady=(10,0))
  self.refresh();root.after(100,self.poll)
 def refresh(self):
  ports=list(list_ports.comports());self.devices=[p.device for p in ports]
  self.ports['values']=[f'{p.device} — {p.description}' for p in ports]
  if ports:self.ports.current(0)
  else:self.port.set('')
 def browse(self):
  v=filedialog.askopenfilename(title='Select Quanta firmware.bin',filetypes=[('Firmware BIN','*.bin')]);
  if v:self.firmware.set(v)
 def logline(self,s):self.q.put(('log',s))
 def start(self,mode):
  if self.busy:return
  port=self.port.get().split(' — ')[0]
  if not port or port not in self.devices:messagebox.showerror('COM port','Select a valid COM port.');return
  fw=Path(self.firmware.get())
  files=[BASE/'bootloader.bin',BASE/'partitions.bin',BASE/'firmware.bin'] if mode=='factory' else [fw]
  if any(not x.is_file() for x in files):messagebox.showerror('Missing file','Required BIN file not found.');return
  if mode=='factory':
   if not messagebox.askyesno('ERASE EVERYTHING','Factory installation permanently erases Wi-Fi, device identity, settings, and stored data. Continue?',icon='warning'):return
  else:
   if not messagebox.askyesno('Firmware update','This updates the app0 firmware and resets OTA boot selection to app0. NVS and SPIFFS remain unchanged. Continue?'):return
  self.busy=True;self.factory.config(state='disabled');self.update.config(state='disabled');self.progress.start(12);self.status.set('Working — do not disconnect USB or power.')
  threading.Thread(target=self.run,args=(port,mode,files),daemon=True).start()
 def command(self,port,*args):
  cmd=[sys.executable,'-m','esptool','--chip','esp32','--port',port,'--baud','115200',*args]
  self.logline('> '+' '.join(cmd[3:]))
  p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1,errors='replace',creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  for line in p.stdout:self.logline(line.rstrip())
  rc=p.wait()
  if rc:raise RuntimeError('Flashing command failed (exit code %d). Check wiring, BOOT/EN, COM port, and power.'%rc)
 def run(self,port,mode,files):
  try:
   self.command(port,'read_mac')
   if mode=='factory':
    self.command(port,'erase_flash')
    self.command(port,'write_flash','-z','--flash_mode','dio','--flash_freq','40m','--flash_size','8MB','0x1000',str(files[0]),'0x8000',str(files[1]),'0x10000',str(files[2]))
   else:
    # OTA metadata can select app1; clearing only otadata makes the bootloader boot app0.
    # NVS at 0x9000 and SPIFFS at 0x3D0000 remain untouched.
    self.command(port,'erase_region','0xe000','0x2000')
    self.command(port,'write_flash','-z','0x10000',str(files[0]))
   self.q.put(('done','SUCCESS: Firmware installed. Disconnect programmer and restart Quanta.'))
  except Exception as e:self.q.put(('error',str(e)))
 def poll(self):
  try:
   while True:
    kind,msg=self.q.get_nowait()
    if kind=='log':
     self.log.config(state='normal');self.log.insert('end',msg+'\n');self.log.see('end');self.log.config(state='disabled')
    else:
     self.busy=False;self.progress.stop();self.factory.config(state='normal');self.update.config(state='normal');self.status.set(msg)
     if kind=='error':messagebox.showerror('Installation failed',msg)
     else:messagebox.showinfo('Installation complete',msg)
  except queue.Empty:pass
  self.root.after(100,self.poll)
if __name__=='__main__':
 root=tk.Tk();Installer(root);root.mainloop()

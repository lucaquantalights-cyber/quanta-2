#!/usr/bin/env python3
"""Quanta ESP32-WROOM-32E-N8 provisioning. Safe preview by default.
No burn-in or firmware flashing: Quanta-specific firmware is not yet validated.
"""
import argparse,hashlib,json,re,secrets,sys
from pathlib import Path
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[1]
INV=ROOT/'claim_inventory'/'claim_inventory.json'
PROJECT='quanta-f9c4c'; URL='https://quanta-f9c4c-default-rtdb.firebaseio.com'
def die(message):raise SystemExit('ERROR: '+message)
def now():return datetime.now(timezone.utc).isoformat(timespec='seconds')
def get_identity(port):
 try:
  import esptool
 except ImportError:die('Install esptool: python -m pip install esptool')
 import subprocess
 cmd=[sys.executable,'-m','esptool','--chip','esp32','--port',port,'read_mac']
 out=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
 if out.returncode:die('Could not read ESP32 MAC: '+out.stderr[-600:])
 match=re.search(r'(?:MAC|Base MAC):\s*([0-9a-fA-F:]{17})',out.stdout)
 if not match:die('MAC address not found in esptool output: '+out.stdout[-600:])
 mac=match.group(1).upper()
 return mac,mac.replace(':','')[-12:]
def main():
 p=argparse.ArgumentParser(description='Quanta-only ESP32 identity + claim + Firebase device auth')
 p.add_argument('device_id');p.add_argument('--port',help='e.g. COM5; reads MAC from physical ESP32')
 p.add_argument('--mac',help='for offline planning only; not proof of connected board')
 p.add_argument('--service-account',type=Path,help='Quanta-only service account JSON')
 p.add_argument('--apply',action='store_true',help='explicitly create Quanta Auth user and database records')
 a=p.parse_args(); device=a.device_id.strip().lower()
 if not re.fullmatch(r'quantacontroller[1-9]\d*',device):p.error('Expected quantacontroller1, quantacontroller2, ...')
 if a.port and a.mac:p.error('Choose either --port or --mac')
 if not a.port and not a.mac:p.error('Provide --port COMx to read actual ESP32 MAC')
 if a.apply and not a.port:p.error('--apply requires physical ESP32 --port; --mac is preview only')
 mac,chip=get_identity(a.port) if a.port else (a.mac.upper(),a.mac.replace(':','').upper())
 if not re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}',mac):p.error('Invalid MAC address')
 if not INV.exists():die('Missing Quanta claim inventory. Run generate_claim_batch.py first.')
 inventory=json.loads(INV.read_text(encoding='utf-8'));claims=inventory.get('claims',[])
 assigned=[c for c in claims if c.get('deviceId')==device]
 if assigned:
  if len(assigned)!=1 or assigned[0].get('macAddress','').upper()!=mac:die('Existing device assignment conflicts with MAC')
  item=assigned[0]
 else:
  unused=sorted((c for c in claims if c.get('status')=='UNUSED'),key=lambda c:(str(c.get('batch','')),int(c.get('sequence',0))))
  if not unused:die('No UNUSED claim codes. Generate a batch first.')
  item=unused[0]
 code=item['claimCode'];digest=hashlib.sha256(code.encode('ascii')).hexdigest()
 email=f'{device}@devices.quanta.invalid' # Internal Firebase Auth identity, not a customer email.
 print(f'PROJECT: {PROJECT}\nDEVICE: {device}\nCHIP: ESP32-WROOM-32E-N8\nMAC: {mac}\nCHIP ID (derived from MAC): {chip}\nCLAIM: {code}\nAUTH ID: {email}')
 if not a.apply:
  print('DRY RUN ONLY: no local files changed, no Firebase writes, no flashing.');return
 if not a.service_account or not a.service_account.is_file():die('Provide Quanta service account JSON with --service-account')
 info=json.loads(a.service_account.read_text(encoding='utf-8'))
 if info.get('project_id')!=PROJECT:die('Service account is NOT from Quanta. Refusing to proceed.')
 try:
  import firebase_admin
  from firebase_admin import credentials,db,auth
 except ImportError:die('Install Firebase Admin: python -m pip install firebase-admin')
 firebase_admin.initialize_app(credentials.Certificate(info),{'databaseURL':URL,'projectId':PROJECT})
 claimref=db.reference('/deviceClaims/'+device); invref=db.reference('/claimInventory/'+digest)
 if claimref.get() is not None or invref.get() is not None:die('Firebase claim/inventory record already exists. Refusing overwrite.')
 for existing in claims:
  if existing is not item and existing.get('macAddress','').upper()==mac:die('MAC already assigned locally')
 try:
  auth.get_user_by_email(email)
 except auth.UserNotFoundError:pass
 else:die('Firebase Auth identity already exists; refusing overwrite.')
 password=secrets.token_urlsafe(36)
 # Create a separate Firebase Auth account; ESP32 firmware must support email/password sign-in.
 user=auth.create_user(email=email,password=password,display_name=device,disabled=False)
 claim={'codeHash':digest,'status':'unclaimed','deviceId':device,'macAddress':mac,'chipId':chip,'board':'esp32-wroom-32e-n8','deviceUid':user.uid}
 record={'status':'ASSIGNED','deviceId':device,'macAddress':mac,'chipId':chip,'deviceUid':user.uid,'batch':item.get('batch'),'sequence':item.get('sequence')}
 device_record={'macAddress':mac,'chipId':chip,'board':'esp32-wroom-32e-n8','deviceUid':user.uid,'claimed':False}
 try:
  db.reference('/').update({'deviceClaims/'+device:claim,'claimInventory/'+digest:record,'devices/'+device:device_record})
 except Exception as exc:
  die('Auth user was created, but database write failed. DO NOT retry blindly. Inspect Quanta Auth user '+user.uid+'; '+str(exc))
 item.update({'status':'ASSIGNED','deviceId':device,'macAddress':mac,'chipId':chip,'firebaseStatus':'VERIFIED','assignedAt':now()})
 INV.with_suffix('.json.bak').write_bytes(INV.read_bytes());inventory['updatedAt']=now();INV.write_text(json.dumps(inventory,indent=2)+'\n',encoding='utf-8')
 # Sensitive credentials: private file, never add to public/ or Firebase RTDB.
 private=ROOT/'secrets'/'device_credentials';private.mkdir(parents=True,exist_ok=True)
 dest=private/(device+'.json')
 if dest.exists():die('Credential output already exists; manual recovery required')
 dest.write_text(json.dumps({'projectId':PROJECT,'deviceId':device,'deviceUid':user.uid,'email':email,'password':password,'macAddress':mac},indent=2)+'\n',encoding='utf-8')
 labels=ROOT/'labels';labels.mkdir(exist_ok=True)
 (labels/(device+'.txt')).write_text(f'Quanta Lights\nDevice ID: {device}\nClaim Code: {code}\nMAC: {mac}\nSetup: /setup.html\n',encoding='utf-8')
 print('PROVISIONED Quanta ONLY. Private device credentials:',dest)
 print('No firmware was built or flashed. Do not ship until firmware/device login and burn-in are tested.')
if __name__=='__main__':main()

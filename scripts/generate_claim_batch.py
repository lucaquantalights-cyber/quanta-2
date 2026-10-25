#!/usr/bin/env python3
"""Quanta-only local claim code generator. Never contacts Firebase."""
import argparse,csv,json,secrets
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
INV=ROOT/'claim_inventory'/'claim_inventory.json'
CSV=ROOT/'claim_inventory'/'claim_codes_to_print.csv'
ABC='ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
def stamp(): return datetime.now(timezone.utc).isoformat(timespec='seconds')
def main():
 p=argparse.ArgumentParser();p.add_argument('--count',type=int,default=100);p.add_argument('--batch',default=datetime.now().strftime('BATCH-%Y%m%d'));a=p.parse_args()
 if not 1<=a.count<=10000:p.error('count must be 1..10000')
 INV.parent.mkdir(parents=True,exist_ok=True)
 data=json.loads(INV.read_text(encoding='utf-8')) if INV.exists() else {'schemaVersion':1,'claims':[]}
 claims=data.setdefault('claims',[]); existing={x['claimCode'] for x in claims}; seq=max((int(x.get('sequence',0)) for x in claims if x.get('batch')==a.batch),default=0)
 for _ in range(a.count):
  while True:
   code=''.join(secrets.choice(ABC) for _ in range(4))+'-'+''.join(secrets.choice(ABC) for _ in range(4))
   if code not in existing:break
  seq+=1;existing.add(code);claims.append({'claimCode':code,'batch':a.batch,'sequence':seq,'status':'UNUSED','createdAt':stamp(),'deviceId':None,'macAddress':None,'chipId':None,'firebaseStatus':'NOT_ASSIGNED'})
 if INV.exists():INV.with_suffix('.json.bak').write_bytes(INV.read_bytes())
 data['updatedAt']=stamp();INV.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
 with CSV.open('w',newline='',encoding='utf-8-sig') as f:
  w=csv.DictWriter(f,fieldnames=['batch','sequence','claimCode','status','deviceId','macAddress','chipId']);w.writeheader();w.writerows([{k:c.get(k,'') for k in w.fieldnames} for c in claims])
 print(f'Generated {a.count} Quanta codes. Local inventory: {INV}\nPrivate print CSV: {CSV}')
if __name__=='__main__':main()

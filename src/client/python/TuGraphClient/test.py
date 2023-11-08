from TuGraphClient import TuGraphClient, AsyncTuGraphClient
import json

client = TuGraphClient("127.0.0.1:7070" , "admin", "73@TuGraph", "tx_graph")
input_filename = '/home/tugraph-db/build/output/lgraph_import_case/0_experiment/input/3hop_stmt.sh'
f = open(input_filename, 'r')
count = 0
total_time = 0.0
for line in iter(f):
    cypher = str(line)
    count += 1
    print(f'{count}')
    res :dict  = client.call_cypher(cypher=cypher, raw_output=False)
    total_time += float(res['elapsed'])
    
print(f'total time: {total_time}')    
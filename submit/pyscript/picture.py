import pandas as pd
import matplotlib.pyplot as plt

# 读取CSV文件
file_path = './build/latency.csv'  # 替换为你的CSV文件路径
df = pd.read_csv(file_path, header=None, names=['Index', 'Col2', 'Col3', 'Latency'])

th = 400000000

# 将延迟从皮秒转换为毫秒
df['Latency'] = df['Latency'] / 1e6

# 统计延迟大于400毫秒的点的数量
outliers = df[df['Latency'] > th]
outlier_count = len(outliers)
print(f"Number of points with latency greater than th: {outlier_count}")

# 过滤掉延迟大于400毫秒的点
df = df[df['Latency'] <= th]

# 绘制图像
plt.figure(figsize=(10, 6))
plt.scatter(df['Index'], df['Latency'], color='red', s=1)
plt.xlabel('Request (th)')
plt.ylabel('Latency (us)')
plt.title('Latency over progress')
plt.grid(True)

# 保存图像
plt.savefig('./pyscript/latency.png')

plt.show()
